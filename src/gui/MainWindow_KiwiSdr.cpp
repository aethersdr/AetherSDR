#include "MainWindow.h"

#include "AppletPanel.h"
#include "KiwiSdrApplet.h"
#include "PanadapterApplet.h"
#include "RxApplet.h"
#include "PanadapterStack.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "SMeterWidget.h"
#include "VfoWidget.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrClient.h"
#include "core/KiwiSdrManager.h"
#include "core/KiwiSdrProtocol.h"
#include "core/LogManager.h"
#include "models/BandSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {

constexpr double kKiwiSdrWaterfallFullBandwidthMhz = 30.0;

QString kiwiConnectionOverlayDetail(KiwiSdrClient::State state,
                                     const QString& detail)
{
    const QString trimmed = detail.trimmed();
    switch (state) {
    case KiwiSdrClient::State::Connecting:
        return trimmed.isEmpty() ? QStringLiteral("Connecting") : trimmed;
    case KiwiSdrClient::State::Busy:
        return trimmed.isEmpty()
            ? QStringLiteral("All KiwiSDR receiver channels are busy")
            : trimmed;
    case KiwiSdrClient::State::Waiting:
        return trimmed.isEmpty()
            ? QStringLiteral("Waiting for a free KiwiSDR receiver slot")
            : trimmed;
    case KiwiSdrClient::State::Camping:
        return trimmed.isEmpty()
            ? QStringLiteral("Monitoring another KiwiSDR receiver channel; "
                             "normal tuning is disabled")
            : trimmed;
    case KiwiSdrClient::State::CampDisconnected:
        return trimmed.isEmpty()
            ? QStringLiteral("The monitored KiwiSDR receiver channel disconnected")
            : trimmed;
    case KiwiSdrClient::State::Error:
        return trimmed.isEmpty() ? QStringLiteral("Connection error") : trimmed;
    case KiwiSdrClient::State::Connected:
        return QString();
    case KiwiSdrClient::State::Disconnected:
        return trimmed.isEmpty() ? QStringLiteral("Disconnected") : trimmed;
    }
    return trimmed.isEmpty() ? QStringLiteral("Disconnected") : trimmed;
}

bool kiwiProfileCanDriveWaterfall(const KiwiSdrManager* manager,
                                  const QString& profileId)
{
    if (!manager || profileId.isEmpty()) {
        return false;
    }

    return KiwiSdrClient::stateAllowsReceiverControl(manager->state(profileId))
        && manager->waterfallAvailable(profileId);
}

QString kiwiApiPolicyText(KiwiSdrProtocol::ApiPolicy policy)
{
    switch (policy) {
    case KiwiSdrProtocol::ApiPolicy::Disabled:
        return QStringLiteral("API disabled");
    case KiwiSdrProtocol::ApiPolicy::Limited:
        return QStringLiteral("API limited");
    case KiwiSdrProtocol::ApiPolicy::Open:
        return QStringLiteral("API open");
    case KiwiSdrProtocol::ApiPolicy::Unknown:
        break;
    }
    return QStringLiteral("API policy unknown");
}

QString kiwiStateName(KiwiSdrClient::State state)
{
    switch (state) {
    case KiwiSdrClient::State::Disconnected:
        return QStringLiteral("disconnected");
    case KiwiSdrClient::State::Connecting:
        return QStringLiteral("connecting");
    case KiwiSdrClient::State::Connected:
        return QStringLiteral("connected");
    case KiwiSdrClient::State::Busy:
        return QStringLiteral("busy");
    case KiwiSdrClient::State::Waiting:
        return QStringLiteral("waiting");
    case KiwiSdrClient::State::Camping:
        return QStringLiteral("camping");
    case KiwiSdrClient::State::CampDisconnected:
        return QStringLiteral("camp_disconnected");
    case KiwiSdrClient::State::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("disconnected");
}

QString kiwiReceiverMetadataSummary(
    const KiwiSdrProtocol::ReceiverMetadata& metadata)
{
    QStringList parts;
    if (!metadata.serverVersion.isEmpty()) {
        parts << QStringLiteral("v%1").arg(metadata.serverVersion);
    }
    if (metadata.hasUsers && metadata.hasUsersMax) {
        parts << QStringLiteral("Users %1/%2")
                     .arg(metadata.users)
                     .arg(metadata.usersMax);
    } else if (metadata.hasUsers) {
        parts << QStringLiteral("Users %1").arg(metadata.users);
    }
    if (metadata.hasBusy && metadata.busy) {
        parts << QStringLiteral("Busy");
    }
    if (metadata.hasCampStatus
        && metadata.campStatus != KiwiSdrProtocol::CampStatus::Unknown) {
        switch (metadata.campStatus) {
        case KiwiSdrProtocol::CampStatus::Offered:
            parts << QStringLiteral("Monitor offered");
            break;
        case KiwiSdrProtocol::CampStatus::Queued:
            if (metadata.hasCampQueuePosition
                && metadata.hasCampQueueWaiters) {
                parts << QStringLiteral("Queue %1/%2")
                             .arg(metadata.campQueuePosition)
                             .arg(metadata.campQueueWaiters);
            } else {
                parts << QStringLiteral("Queued");
            }
            if (metadata.hasCampQueueReloadRecommended
                && metadata.campQueueReloadRecommended) {
                parts << QStringLiteral("Channel free");
            }
            break;
        case KiwiSdrProtocol::CampStatus::Accepted:
            parts << (metadata.hasCampReceiverChannel
                          ? QStringLiteral("Camping RX%1")
                                .arg(metadata.campReceiverChannel)
                          : QStringLiteral("Camping"));
            break;
        case KiwiSdrProtocol::CampStatus::Rejected:
            parts << QStringLiteral("Camping rejected");
            break;
        case KiwiSdrProtocol::CampStatus::AudioStopped:
            parts << QStringLiteral("Camp audio stopped");
            break;
        case KiwiSdrProtocol::CampStatus::Disconnected:
            parts << QStringLiteral("Camp disconnected");
            break;
        case KiwiSdrProtocol::CampStatus::Unknown:
            break;
        }
    }
    if (metadata.hasMaxCampers) {
        parts << QStringLiteral("Max campers %1").arg(metadata.maxCampers);
    }
    if (metadata.hasExtApi) {
        parts << QStringLiteral("%1 (%2)")
                     .arg(kiwiApiPolicyText(metadata.apiPolicy))
                     .arg(metadata.extApi);
    }
    if (metadata.hasGpsGood) {
        parts << (metadata.gpsGood ? QStringLiteral("GPS good")
                                   : QStringLiteral("GPS not good"));
    } else if (!metadata.gpsStatus.isEmpty()) {
        parts << QStringLiteral("GPS %1").arg(metadata.gpsStatus);
    }
    if (metadata.hasAdcClipping) {
        parts << (metadata.adcClipping ? QStringLiteral("ADC clipping")
                                       : QStringLiteral("ADC normal"));
    }
    if (metadata.hasCoverageCenter && metadata.hasCoverageBandwidth) {
        const double lowMhz =
            metadata.coverageCenterMhz - metadata.coverageBandwidthMhz * 0.5;
        const double highMhz =
            metadata.coverageCenterMhz + metadata.coverageBandwidthMhz * 0.5;
        parts << QStringLiteral("%1-%2 MHz")
                     .arg(lowMhz, 0, 'f', 3)
                     .arg(highMhz, 0, 'f', 3);
    } else if (metadata.hasReportedFrequency) {
        parts << QStringLiteral("%1 MHz")
                     .arg(metadata.reportedFrequencyKhz / 1000.0, 0, 'f', 3);
    }
    if (metadata.hasReceiverChannel && metadata.hasWaterfallChannels) {
        parts << QStringLiteral("RX slot %1, W/F %2")
                     .arg(metadata.receiverChannel + 1)
                     .arg(metadata.waterfallChannels);
    }
    return parts.join(QStringLiteral(" · "));
}

QString kiwiStreamSummary(const KiwiSdrProtocol::StreamCapability& capability)
{
    const bool hasProtocolData =
        capability.requested
        || capability.observed
        || capability.uncompressedRequested
        || capability.compressedRequested
        || capability.uncompressedObserved
        || capability.compressedObserved
        || !capability.supportedLayouts.isEmpty()
        || !capability.observedLayouts.isEmpty()
        || capability.lastObservedLayout != KiwiSdrProtocol::FrameLayout::Unknown
        || !capability.unsupportedReason.isEmpty();
    if (!hasProtocolData) {
        return QString();
    }

    QStringList parts;
    parts << KiwiSdrProtocol::streamModeName(capability.mode);
    if (capability.observed
        && capability.lastObservedLayout != KiwiSdrProtocol::FrameLayout::Unknown) {
        parts << KiwiSdrProtocol::frameLayoutName(capability.lastObservedLayout);
    } else if (capability.requested) {
        parts << QStringLiteral("requested");
    }
    if (capability.uncompressedRequested && !capability.uncompressedObserved) {
        parts << QStringLiteral("uncompressed requested");
    } else if (capability.uncompressedObserved) {
        parts << QStringLiteral("uncompressed observed");
    }
    if (capability.compressedRequested && !capability.compressedObserved) {
        parts << QStringLiteral("compressed requested");
    }
    if (capability.compressedObserved) {
        parts << QStringLiteral("compressed observed");
    }
    if (!capability.unsupportedReason.isEmpty()) {
        parts << QStringLiteral("unsupported: %1")
                     .arg(capability.unsupportedReason);
    }
    return parts.join(QStringLiteral(" "));
}

QString kiwiProtocolSummary(const KiwiSdrProtocol::ProtocolState& protocol)
{
    QStringList parts;
    if (protocol.authMode != KiwiSdrProtocol::AuthMode::Unknown) {
        parts << QStringLiteral("Auth %1")
                     .arg(KiwiSdrProtocol::authModeName(protocol.authMode));
    }
    const QString sound = kiwiStreamSummary(protocol.sound);
    if (!sound.isEmpty()) {
        parts << sound;
    }
    const QString waterfall = kiwiStreamSummary(protocol.waterfall);
    if (!waterfall.isEmpty()) {
        parts << waterfall;
    }
    if (!protocol.unsupportedFrames.isEmpty()) {
        const KiwiSdrProtocol::FrameObservation last =
            protocol.unsupportedFrames.last();
        if (!last.unsupportedReason.isEmpty()) {
            parts << QStringLiteral("Last skipped %1")
                         .arg(last.unsupportedReason);
        }
    }
    return parts.join(QStringLiteral(" · "));
}

QJsonArray kiwiFrameLayoutsToJson(const QVector<KiwiSdrProtocol::FrameLayout>& layouts)
{
    QJsonArray array;
    for (KiwiSdrProtocol::FrameLayout layout : layouts) {
        array.append(KiwiSdrProtocol::frameLayoutName(layout));
    }
    return array;
}

QJsonObject kiwiStreamCapabilityToJson(
    const KiwiSdrProtocol::StreamCapability& capability)
{
    return QJsonObject{
        {QStringLiteral("mode"), KiwiSdrProtocol::streamModeName(capability.mode)},
        {QStringLiteral("requested"), capability.requested},
        {QStringLiteral("observed"), capability.observed},
        {QStringLiteral("uncompressedRequested"),
            capability.uncompressedRequested},
        {QStringLiteral("compressedRequested"),
            capability.compressedRequested},
        {QStringLiteral("uncompressedObserved"),
            capability.uncompressedObserved},
        {QStringLiteral("compressedObserved"), capability.compressedObserved},
        {QStringLiteral("supportedLayouts"),
            kiwiFrameLayoutsToJson(capability.supportedLayouts)},
        {QStringLiteral("observedLayouts"),
            kiwiFrameLayoutsToJson(capability.observedLayouts)},
        {QStringLiteral("lastObservedLayout"),
            KiwiSdrProtocol::frameLayoutName(capability.lastObservedLayout)},
        {QStringLiteral("unsupportedReason"), capability.unsupportedReason},
    };
}

QJsonObject kiwiMetadataToJson(
    const KiwiSdrProtocol::ReceiverMetadata& metadata)
{
    QJsonObject json{
        {QStringLiteral("apiPolicy"),
            KiwiSdrProtocol::apiPolicyName(metadata.apiPolicy)},
        {QStringLiteral("serverHeader"), metadata.serverHeader},
        {QStringLiteral("serverVersion"), metadata.serverVersion},
        {QStringLiteral("serverBuild"), metadata.serverBuild},
        {QStringLiteral("gpsStatus"), metadata.gpsStatus},
    };
    if (metadata.hasUsers) {
        json[QStringLiteral("users")] = metadata.users;
    }
    if (metadata.hasUsersMax) {
        json[QStringLiteral("usersMax")] = metadata.usersMax;
    }
    if (metadata.hasPreempt) {
        json[QStringLiteral("preempt")] = metadata.preempt;
    }
    if (metadata.hasExtApi) {
        json[QStringLiteral("extApi")] = metadata.extApi;
    }
    if (metadata.hasBusy) {
        json[QStringLiteral("busy")] = metadata.busy;
    }
    if (metadata.hasCampStatus) {
        json[QStringLiteral("campStatus")] =
            KiwiSdrProtocol::campStatusName(metadata.campStatus);
    }
    if (metadata.hasCampReceiverChannel) {
        json[QStringLiteral("campReceiverChannel")] =
            metadata.campReceiverChannel;
    }
    if (metadata.hasCampQueuePosition) {
        json[QStringLiteral("campQueuePosition")] =
            metadata.campQueuePosition;
    }
    if (metadata.hasCampQueueWaiters) {
        json[QStringLiteral("campQueueWaiters")] =
            metadata.campQueueWaiters;
    }
    if (metadata.hasCampQueueReloadRecommended) {
        json[QStringLiteral("campQueueReloadRecommended")] =
            metadata.campQueueReloadRecommended;
    }
    if (metadata.hasMaxCampers) {
        json[QStringLiteral("maxCampers")] = metadata.maxCampers;
    }
    if (metadata.hasGpsGood) {
        json[QStringLiteral("gpsGood")] = metadata.gpsGood;
    }
    if (metadata.hasAdcClipping) {
        json[QStringLiteral("adcClipping")] = metadata.adcClipping;
    }
    if (metadata.hasReportedFrequency) {
        json[QStringLiteral("reportedFrequencyKhz")] =
            metadata.reportedFrequencyKhz;
    }
    if (metadata.hasCoverageCenter) {
        json[QStringLiteral("coverageCenterMhz")] = metadata.coverageCenterMhz;
    }
    if (metadata.hasCoverageBandwidth) {
        json[QStringLiteral("coverageBandwidthMhz")] =
            metadata.coverageBandwidthMhz;
    }
    if (metadata.hasReceiverChannel) {
        json[QStringLiteral("receiverChannel")] = metadata.receiverChannel;
    }
    if (metadata.hasWaterfallChannels) {
        json[QStringLiteral("waterfallChannels")] = metadata.waterfallChannels;
    }
    QJsonArray fields;
    for (const QString& field : metadata.stableStatusFields) {
        fields.append(field);
    }
    json[QStringLiteral("stableStatusFields")] = fields;
    return json;
}

QJsonObject kiwiProtocolToJson(
    const KiwiSdrProtocol::ProtocolState& protocol)
{
    QJsonArray unsupportedFeatures;
    for (const QString& reason : protocol.unsupportedFeatureReasons) {
        unsupportedFeatures.append(reason);
    }
    QJsonArray unsupportedFrames;
    for (const KiwiSdrProtocol::FrameObservation& frame :
         protocol.unsupportedFrames) {
        unsupportedFrames.append(QJsonObject{
            {QStringLiteral("stream"),
                KiwiSdrProtocol::streamModeName(frame.stream)},
            {QStringLiteral("layout"),
                KiwiSdrProtocol::frameLayoutName(frame.layout)},
            {QStringLiteral("frameBytes"), frame.frameBytes},
            {QStringLiteral("payloadBytes"), frame.payloadBytes},
            {QStringLiteral("supported"), frame.supported},
            {QStringLiteral("reason"), frame.unsupportedReason},
        });
    }
    return QJsonObject{
        {QStringLiteral("serverVersion"), protocol.serverVersion},
        {QStringLiteral("serverBuild"), protocol.serverBuild},
        {QStringLiteral("authMode"),
            KiwiSdrProtocol::authModeName(protocol.authMode)},
        {QStringLiteral("apiPolicy"),
            KiwiSdrProtocol::apiPolicyName(protocol.apiPolicy)},
        {QStringLiteral("extApi"), protocol.extApi},
        {QStringLiteral("sound"),
            kiwiStreamCapabilityToJson(protocol.sound)},
        {QStringLiteral("waterfall"),
            kiwiStreamCapabilityToJson(protocol.waterfall)},
        {QStringLiteral("unsupportedFeatures"), unsupportedFeatures},
        {QStringLiteral("unsupportedFrames"), unsupportedFrames},
    };
}

void restoreFlexPanadapterDisplayRange(RadioModel& radioModel,
                                       const QString& panId,
                                       SpectrumWidget* spectrum)
{
    if (!spectrum) {
        return;
    }

    if (PanadapterModel* pan = radioModel.panadapter(panId)) {
        if (pan->panStreamId() && radioModel.panStream()) {
            radioModel.panStream()->setDbmRange(
                pan->panStreamId(), pan->minDbm(), pan->maxDbm());
        }
        spectrum->setDbmRange(pan->minDbm(), pan->maxDbm());
    }

    spectrum->prepareForFftScaleChange();
    spectrum->reacquireNoiseFloorLock();
}

void setKiwiSdrWaterfallActive(RadioModel& radioModel,
                               const QString& panId,
                               SpectrumWidget* spectrum,
                               bool active)
{
    if (!spectrum) {
        return;
    }

    const bool wasActive = spectrum->kiwiSdrWaterfallActive();
    spectrum->setKiwiSdrWaterfallActive(active);
    if (wasActive && !active) {
        restoreFlexPanadapterDisplayRange(radioModel, panId, spectrum);
    }
}

int kiwiSdrAgcThresholdDbForSliceValue(int value)
{
    return qBound(KiwiSdrProtocol::kAgcThresholdMinDb, value,
                  KiwiSdrProtocol::kAgcThresholdMaxDb);
}

int kiwiSdrSquelchThresholdDbForSliceValue(int value)
{
    return KiwiSdrProtocol::squelchSliderLevelToMarginDb(value);
}

bool kiwiSdrModeUsesNbfmSquelch(const QString& mode)
{
    const QString normalized = mode.trimmed().toUpper();
    return normalized == QStringLiteral("FM")
        || normalized == QStringLiteral("NFM");
}

KiwiSdrReceiverControls kiwiSdrReceiverControlsForSlice(
    const SliceModel& slice,
    int autoSqlMarginDb)
{
    const QString mode = slice.receiveAgcMode().trimmed().toLower();
    KiwiSdrReceiverControls controls;
    controls.agcEnabled = mode != QStringLiteral("off");
    controls.agcGainDb = qBound(0, slice.receiveAgcOffLevel(), 100);
    controls.agcHang = false;
    controls.agcThresholdDb =
        kiwiSdrAgcThresholdDbForSliceValue(slice.receiveAgcThreshold());
    controls.agcDecayMs = KiwiSdrProtocol::agcDecayMsForMode(mode);
    controls.squelchEnabled = slice.receiveSquelchOn();
    controls.squelchThresholdDb = autoSqlMarginDb > 0
        ? qBound(1, autoSqlMarginDb, 20)
        : kiwiSdrSquelchThresholdDbForSliceValue(slice.receiveSquelchLevel());
    if (autoSqlMarginDb <= 0 && kiwiSdrModeUsesNbfmSquelch(slice.mode())) {
        controls.squelchThresholdDb = qBound(
            1, slice.receiveSquelchLevel(),
            KiwiSdrProtocol::kSquelchUiMaxLevel);
    }
    return controls;
}

} // namespace

SliceModel* MainWindow::kiwiSdrAudioTargetSlice() const
{
    if (SliceModel* slice = activeSlice()) {
        if (m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            return slice;
        }
    }

    if (m_kiwiSdrTrackedSliceId >= 0) {
        if (SliceModel* slice = m_radioModel.slice(m_kiwiSdrTrackedSliceId)) {
            if (m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
                return slice;
            }
        }
    }

    for (SliceModel* slice : m_radioModel.slices()) {
        if (slice && m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            return slice;
        }
    }

    return nullptr;
}

void MainWindow::setKiwiSdrVirtualAntennaForSlice(int sliceId,
                                                  const QString& profileId)
{
    // User/automation menu selection: also selects the slice (selectSlice=true).
    setKiwiSdrVirtualAntennaForSliceInternal(sliceId, profileId,
                                             /*selectSlice=*/true);
}

void MainWindow::setKiwiSdrVirtualAntennaForSliceInternal(int sliceId,
                                                          const QString& profileId,
                                                          bool selectSlice)
{
    if (!m_kiwiSdrManager || profileId.isEmpty()) {
        return;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)) {
        return;
    }
    m_kiwiSdrBandRecallPreparations.remove(sliceId);

    // Selecting a receive source from a VFO/header menu is also selecting the
    // slice whose receive path is being changed. Keep the side RX applet bound
    // to that slice so SQL/AGC controls drive the Kiwi replacement source, not
    // whichever Flex slice happened to be active before the menu action.
    // Automatic re-arms (band-recall finish, #4158 recreation re-bind) pass
    // selectSlice=false: they must not steal the active-slice selection when the
    // Kiwi slice lives on a non-active pan — the applet binding already exists
    // from the original user selection.
    if (selectSlice && sliceId != m_activeSliceId) {
        setActiveSliceInternal(sliceId, false);
    }

    if (!m_kiwiSdrVirtualPreviousMute.contains(sliceId)) {
        m_kiwiSdrVirtualPreviousMute.insert(sliceId, slice->flexAudioMute());
    }
    const bool wasExternalReceiveActive = slice->externalReceiveReplacementActive();
    slice->setExternalReceiveAudioReplacementMute(true);
    if (!wasExternalReceiveActive && slice->isDiversityChild()
        && slice->flexAudioGain() <= 0.0f) {
        // A Flex diversity child always reports audio_level=0 (its audio is
        // combined into the parent), so the snapshot just taken above is
        // silence. Seed it from the parent's level instead, so both slices
        // start matched rather than the child starting muted — see #4300.
        for (SliceModel* other : m_radioModel.slices()) {
            if (isSameDiversityReceivePair(slice, other) && other->isDiversityParent()) {
                slice->setAudioGain(other->audioGain());
                break;
            }
        }
    }
    if (m_appletPanel) {
        m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
    }

    // Receive-only: route audio from the Kiwi profile and mute Flex audio for
    // this slice. No antenna command is sent to the radio (Principle I).
    qCInfo(lcKiwiSdr).noquote()
        << "Virtual RX antenna selected for slice" << sliceId
        << "profile=" << m_kiwiSdrManager->displayName(profileId)
        << "(Flex audio muted, no radio command sent)";

    m_kiwiSdrManager->assignSliceToProfile(
        sliceId, profileId, slice->frequency(), slice->mode(),
        slice->filterLow(), slice->filterHigh(), slice->panId(),
        BandSettings::bandForFrequency(slice->frequency()));
    updateKiwiSdrVirtualTrackingForSlice(slice);
    updateKiwiSdrVirtualAudioControlsForSlice(slice);
    updateKiwiSdrVirtualReceiverControlsForSlice(slice);
    syncActiveSliceSquelchLineToSpectrums();
    syncActiveSliceAutoSquelchToSpectrums();
    syncFlexRxPanToAudioEngine();

    if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
        if (kiwiSdrDisplaySliceForPan(slice->panId()) == slice) {
            spectrum->setKiwiSdrWaterfallAvailable(
                m_kiwiSdrManager->waterfallAvailable(profileId));
            spectrum->setKiwiSdrWaterfallProfile(profileId);
            spectrum->setKiwiSdrWaterfallActive(
                kiwiSdrPanDisplaysKiwi(slice->panId()) &&
                kiwiProfileCanDriveWaterfall(m_kiwiSdrManager, profileId));
        }
        if (VfoWidget* vfo = spectrum->vfoWidget(slice->sliceId())) {
            vfo->setReceiveMeterReading(
                KiwiSdrProtocol::meterUnavailable(
                    KiwiSdrProtocol::MeterSource::Unknown,
                    QStringLiteral("Waiting for KiwiSDR meter data")));
        }
    }
    if (slice->sliceId() == m_activeSliceId && m_appletPanel
        && m_appletPanel->sMeterWidget()) {
        m_appletPanel->sMeterWidget()->setReceiveMeterReading(
            KiwiSdrProtocol::meterUnavailable(
                KiwiSdrProtocol::MeterSource::Unknown,
                QStringLiteral("Waiting for KiwiSDR meter data")));
    }
    syncKiwiSdrPanadapterUiState(slice->panId());
    syncActiveSliceAutoSquelchToSpectrums();
    scheduleKiwiSdrUiSync(KiwiSdrUiSyncDiversityEsc
                          | KiwiSdrUiSyncWaterfallAvailability);
}

void MainWindow::clearKiwiSdrVirtualAntennaForSlice(int sliceId)
{
    qCInfo(lcKiwiSdr).noquote()
        << "Virtual RX antenna cleared for slice" << sliceId
        << "(Flex audio restored)";
    m_kiwiSdrBandRecallPreparations.remove(sliceId);
    QString panId;
    if (SliceModel* slice = m_radioModel.slice(sliceId)) {
        panId = slice->panId();
        const bool restoreMute =
            m_kiwiSdrVirtualPreviousMute.contains(sliceId)
                ? m_kiwiSdrVirtualPreviousMute.take(sliceId)
                : slice->flexAudioMute();
        slice->setExternalReceiveAudioReplacementMute(false, restoreMute);
        if (m_appletPanel) {
            m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
        }
        if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
            setKiwiSdrWaterfallActive(m_radioModel, panId, spectrum, false);
            spectrum->setKiwiSdrConnectionOverlay(false);
            spectrum->setKiwiSdrWaterfallProfile(QString());
        }
    }

    if (!panId.isEmpty()) {
        m_kiwiSdrFlexDisplayPans.remove(panId);
    }
    if (m_kiwiSdrManager) {
        m_kiwiSdrManager->clearSliceAssignment(sliceId);
    }
    syncFlexRxPanToAudioEngine();
    syncActiveSliceSquelchLineToSpectrums();
    syncActiveSliceAutoSquelchToSpectrums();
    scheduleKiwiSdrUiSync(KiwiSdrUiSyncDiversityEsc
                          | KiwiSdrUiSyncWaterfallAvailability);
    if (!panId.isEmpty()) {
        syncKiwiSdrPanadapterUiState(panId);
    }
}

void MainWindow::prepareKiwiSdrBandRecallForPan(const QString& panId)
{
    if (!m_kiwiSdrManager || panId.isEmpty()) {
        return;
    }

    // A deferred request can supersede an older one for the same pan. Re-arm
    // any still-prepared slice before beginning the new atomic command pair.
    // The #4158/Center Lock rebind marker (noteBandRecallForPan) is owned by the
    // band-recall trigger sites, not here — this path only performs the #4209
    // audio-mute handoff, bracketed around the actual wire dispatch.
    finishPreparedKiwiSdrBandRecallForPan(panId);

    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())
            || !slice->externalReceiveReplacementActive()) {
            continue;
        }

        const QString profileId =
            m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
        if (profileId.isEmpty()) {
            continue;
        }

        const bool restoreMute =
            m_kiwiSdrVirtualPreviousMute.contains(slice->sliceId())
                ? m_kiwiSdrVirtualPreviousMute.take(slice->sliceId())
                : slice->flexAudioMute();
        m_kiwiSdrBandRecallPreparations.insert(
            slice->sliceId(), {panId, profileId});
        slice->prepareExternalReceiveAudioReplacementBandRecall(restoreMute);
        qCInfo(lcKiwiSdr).noquote()
            << "Preparing KiwiSDR band recall"
            << "pan=" << panId
            << "slice=" << slice->sliceId()
            << "restore_flex_mute=" << (restoreMute ? 1 : 0);
    }

    // Same-band recalls may not change any slice field, while some firmware
    // drops and recreates the slice. A recreation re-arms via the #4158 rebind;
    // normal retunes re-arm from frequencyChanged; this grace fallback covers
    // only the no-change case, sharing the band-recall recreation window. Guard
    // on a per-pan epoch so a rapid second recall (which bumps the generation)
    // can't have this now-stale timer finish its in-flight handoff early. The
    // marker's own lifecycle is owned by noteBandRecallForPan, so this timer
    // touches only the mute handoff.
    const quint64 generation = ++m_kiwiSdrBandRecallGenerations[panId];
    QTimer::singleShot(kBandRecallRecreateGraceMs, this, [this, panId, generation]() {
        if (m_kiwiSdrBandRecallGenerations.value(panId) != generation) {
            return;
        }
        finishPreparedKiwiSdrBandRecallForPan(panId);
    });
}

// Returns true only when the preparation was consumed AND re-armed — i.e.
// setKiwiSdrVirtualAntennaForSlice() ran (which also performs the tracking
// update). A stale-preparation guard miss returns false so the caller can fall
// back to its normal tracking path instead of assuming the re-entrant re-arm.
bool MainWindow::finishPreparedKiwiSdrBandRecallForSlice(SliceModel* slice)
{
    if (!m_kiwiSdrManager || !slice) {
        return false;
    }

    const auto it = m_kiwiSdrBandRecallPreparations.find(slice->sliceId());
    if (it == m_kiwiSdrBandRecallPreparations.end()) {
        return false;
    }

    const KiwiSdrBandRecallPreparation preparation = it.value();
    const QString assignedProfile =
        m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
    m_kiwiSdrBandRecallPreparations.erase(it);
    if (slice->panId() != preparation.panId
        || assignedProfile != preparation.profileId) {
        return false;
    }

    // SliceModel emits frequencyChanged only after the complete radio delta —
    // including audio_mute — has been applied. setKiwi... therefore snapshots
    // the recalled band's own mute before re-arming Flex suppression. This
    // finish is automatic, so selectSlice=false — it must not steal the active
    // slice when the recalled Kiwi slice is on a non-active pan.
    const bool recalledFlexMute = slice->flexAudioMute();
    setKiwiSdrVirtualAntennaForSliceInternal(slice->sliceId(),
                                             preparation.profileId,
                                             /*selectSlice=*/false);
    qCInfo(lcKiwiSdr).noquote()
        << "KiwiSDR band recall re-armed"
        << "pan=" << preparation.panId
        << "slice=" << slice->sliceId()
        << "recalled_flex_mute=" << (recalledFlexMute ? 1 : 0);
    return true;
}

void MainWindow::finishPreparedKiwiSdrBandRecallForPan(const QString& panId)
{
    QVector<int> sliceIds;
    for (auto it = m_kiwiSdrBandRecallPreparations.cbegin();
         it != m_kiwiSdrBandRecallPreparations.cend(); ++it) {
        if (it->panId == panId) {
            sliceIds.append(it.key());
        }
    }

    for (int sliceId : sliceIds) {
        if (SliceModel* slice = m_radioModel.slice(sliceId)) {
            finishPreparedKiwiSdrBandRecallForSlice(slice);
        } else {
            m_kiwiSdrBandRecallPreparations.remove(sliceId);
        }
    }
}

QJsonObject MainWindow::automationSetSliceReceiveSource(const QString& arg)
{
    auto error = [](const QString& message) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), message},
        };
    };

    if (!m_kiwiSdrManager) {
        return error(QStringLiteral("KiwiSDR manager is unavailable"));
    }

    QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return error(QStringLiteral(
            "slice rxsource requires '<slice-id|active> <profile-name|profile-id|flex>'"));
    }

    int sliceId = -1;
    bool okId = false;
    const int parsedSliceId = parts.first().toInt(&okId);
    if (okId) {
        sliceId = parsedSliceId;
        parts.removeFirst();
    } else if (parts.first().compare(QStringLiteral("active"),
                                     Qt::CaseInsensitive) == 0) {
        sliceId = m_activeSliceId;
        parts.removeFirst();
    } else {
        sliceId = m_activeSliceId;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)) {
        return error(QStringLiteral("no controllable slice for rxsource"));
    }
    if (parts.isEmpty()) {
        return error(QStringLiteral("slice rxsource requires a receive source"));
    }

    const QString selector = parts.join(QLatin1Char(' ')).trimmed();
    const QString lower = selector.toLower();
    if (lower == QLatin1String("flex") || lower == QLatin1String("none")
        || lower == QLatin1String("clear")) {
        clearKiwiSdrVirtualAntennaForSlice(sliceId);
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("slice"), QStringLiteral("rxsource")},
            {QStringLiteral("id"), sliceId},
            {QStringLiteral("source"), QStringLiteral("flex")},
            {QStringLiteral("requested"), true},
        };
    }

    QString profileId =
        m_kiwiSdrManager->profileIdForVirtualAntennaToken(selector);
    if (profileId.isEmpty()) {
        for (const KiwiSdrAntennaProfile& profile :
             m_kiwiSdrManager->profiles()) {
            if (profile.id.compare(selector, Qt::CaseInsensitive) == 0
                || profile.name.compare(selector, Qt::CaseInsensitive) == 0
                || m_kiwiSdrManager->displayName(profile.id)
                       .compare(selector, Qt::CaseInsensitive) == 0
                || profile.endpoint.compare(selector, Qt::CaseInsensitive) == 0) {
                profileId = profile.id;
                break;
            }
        }
    }

    if (profileId.isEmpty()) {
        QJsonArray profiles;
        for (const KiwiSdrAntennaProfile& profile :
             m_kiwiSdrManager->profiles()) {
            profiles.append(QJsonObject{
                {QStringLiteral("id"), profile.id},
                {QStringLiteral("name"), m_kiwiSdrManager->displayName(profile.id)},
                {QStringLiteral("endpoint"), profile.endpoint},
            });
        }
        QJsonObject response = error(
            QStringLiteral("unknown KiwiSDR receive source '") + selector
            + QStringLiteral("'"));
        response.insert(QStringLiteral("profiles"), profiles);
        return response;
    }

    setKiwiSdrVirtualAntennaForSlice(sliceId, profileId);
    const KiwiSdrAntennaProfile profile = m_kiwiSdrManager->profile(profileId);
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("slice"), QStringLiteral("rxsource")},
        {QStringLiteral("id"), sliceId},
        {QStringLiteral("source"), QStringLiteral("kiwi")},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("profileName"), m_kiwiSdrManager->displayName(profileId)},
        {QStringLiteral("endpoint"), profile.endpoint},
        {QStringLiteral("requested"), true},
    };
}

QJsonObject MainWindow::automationKiwiSdrSnapshot() const
{
    QJsonArray profiles;
    QJsonArray flexDisplayPans;
    QStringList flexDisplayPanIds = m_kiwiSdrFlexDisplayPans.values();
    std::sort(flexDisplayPanIds.begin(), flexDisplayPanIds.end());
    for (const QString& panId : flexDisplayPanIds) {
        flexDisplayPans.append(panId);
    }
    if (m_kiwiSdrManager) {
        for (const KiwiSdrAntennaProfile& profile :
             m_kiwiSdrManager->profiles()) {
            const KiwiSdrReceiverTelemetry telemetry =
                m_kiwiSdrManager->telemetry(profile.id);
            const int assignedSlice =
                m_kiwiSdrManager->assignedSliceForProfile(profile.id);
            profiles.append(QJsonObject{
                {QStringLiteral("id"), profile.id},
                {QStringLiteral("name"),
                    m_kiwiSdrManager->displayName(profile.id)},
                {QStringLiteral("endpoint"), profile.endpoint},
                {QStringLiteral("autoConnect"), profile.autoConnect},
                {QStringLiteral("state"),
                    kiwiStateName(m_kiwiSdrManager->state(profile.id))},
                {QStringLiteral("detail"),
                    m_kiwiSdrManager->stateDetail(profile.id)},
                {QStringLiteral("assignedSlice"), assignedSlice},
                {QStringLiteral("waterfallAvailable"),
                    m_kiwiSdrManager->waterfallAvailable(profile.id)},
                {QStringLiteral("waterfallDetail"),
                    m_kiwiSdrManager->waterfallDetail(profile.id)},
                {QStringLiteral("metadata"),
                    kiwiMetadataToJson(telemetry.metadata)},
                {QStringLiteral("protocol"),
                    kiwiProtocolToJson(telemetry.protocol)},
                {QStringLiteral("soundSequence"), telemetry.soundSequence},
                {QStringLiteral("soundSequenceGaps"),
                    static_cast<double>(telemetry.soundSequenceGaps)},
                {QStringLiteral("waterfallSequence"),
                    telemetry.waterfallSequence},
                {QStringLiteral("waterfallSequenceGaps"),
                    static_cast<double>(telemetry.waterfallSequenceGaps)},
            });
        }
    }

    return QJsonObject{
        {QStringLiteral("diagnosticSoundCompressionRequested"),
            KiwiSdrClient::diagnosticSoundCompressionRequested()},
        {QStringLiteral("diagnosticWaterfallCompressionRequested"),
            KiwiSdrClient::diagnosticWaterfallCompressionRequested()},
        {QStringLiteral("profiles"), profiles},
        {QStringLiteral("profileCount"), profiles.size()},
        {QStringLiteral("flexDisplayPans"), flexDisplayPans},
    };
}

SliceModel* MainWindow::flexRxPanSourceSlice() const
{
    auto isFlexBacked = [this](const SliceModel* slice) {
        return slice
            && m_radioModel.sliceMayBelongToUs(slice->sliceId())
            && (!m_kiwiSdrManager
                || m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId())
                       .isEmpty());
    };

    SliceModel* active = m_radioModel.slice(m_activeSliceId);
    if (isFlexBacked(active)) {
        return active;
    }

    if (active && active->diversity()) {
        for (SliceModel* slice : m_radioModel.slices()) {
            if (!isFlexBacked(slice) || !slice->diversity()
                || slice->sliceId() == active->sliceId()) {
                continue;
            }
            if ((active->isDiversityChild() && slice->isDiversityParent())
                || (active->isDiversityParent() && slice->isDiversityChild())) {
                return slice;
            }
        }

        for (SliceModel* slice : m_radioModel.slices()) {
            if (isFlexBacked(slice) && slice->diversity()
                && slice->sliceId() != active->sliceId()) {
                return slice;
            }
        }
    }

    SliceModel* onlyUnmutedFlexSlice = nullptr;
    for (SliceModel* slice : m_radioModel.slices()) {
        if (!isFlexBacked(slice) || slice->flexAudioMute()) {
            continue;
        }
        if (onlyUnmutedFlexSlice) {
            return nullptr;
        }
        onlyUnmutedFlexSlice = slice;
    }
    return onlyUnmutedFlexSlice;
}

void MainWindow::syncFlexRxPanToAudioEngine()
{
    if (!m_audio) {
        return;
    }
    if (SliceModel* slice = flexRxPanSourceSlice()) {
        m_audio->setRxPan(slice->flexAudioPan());
    }
}

void MainWindow::updateKiwiSdrVirtualTrackingForSlice(SliceModel* slice)
{
    if (!m_kiwiSdrManager || !slice) {
        return;
    }

    if (finishPreparedKiwiSdrBandRecallForSlice(slice)) {
        // setKiwiSdrVirtualAntennaForSlice() performed the tracking update in
        // its re-entrant call after consuming the preparation. A stale-guard
        // miss returns false and falls through to the normal tracking below.
        return;
    }

    const QString profileId =
        m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
    if (profileId.isEmpty()) {
        return;
    }

    m_kiwiSdrManager->updateSliceTracking(
        slice->sliceId(), slice->frequency(), slice->mode(),
        slice->filterLow(), slice->filterHigh(), slice->panId(),
        BandSettings::bandForFrequency(slice->frequency()));
    if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
        m_kiwiSdrManager->updateWaterfallView(
            slice->sliceId(), slice->panId(), spectrum->centerMhz(),
            spectrum->bandwidthMhz(), spectrum->wfLineDuration());
        if (profileId == kiwiSdrProfileForPan(slice->panId())
            && kiwiSdrPanDisplaysKiwi(slice->panId())
            && kiwiProfileCanDriveWaterfall(m_kiwiSdrManager, profileId)) {
            spectrum->setKiwiSdrWaterfallAvailable(
                m_kiwiSdrManager->waterfallAvailable(profileId));
            spectrum->setKiwiSdrWaterfallProfile(profileId);
            spectrum->setKiwiSdrWaterfallActive(true);
            syncKiwiSdrAppletWaterfallState();
        }
        syncKiwiSdrPanadapterUiState(slice->panId());
    }
}

SliceModel* MainWindow::kiwiSdrDisplaySliceForPan(const QString& panId) const
{
    if (!m_kiwiSdrManager || panId.isEmpty()) {
        return nullptr;
    }

    // Diversity pans have one visual source, and it follows the parent slice.
    // If only the child is using Kiwi, keep the pan/FFT/waterfall on the
    // parent's Flex source while the child contributes audio only.
    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())
            || !slice->isDiversityParent()) {
            continue;
        }
        return m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId())
                   .isEmpty()
            ? nullptr
            : slice;
    }

    QVector<SliceModel*> candidates;
    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }
        if (!m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId())
                 .isEmpty()) {
            candidates.append(slice);
        }
    }
    if (candidates.isEmpty()) {
        return nullptr;
    }

    if (SliceModel* active = activeSlice()) {
        for (SliceModel* slice : candidates) {
            if (slice == active) {
                return slice;
            }
        }
    }

    SliceModel* indexedDiversitySlice = nullptr;
    for (SliceModel* slice : candidates) {
        if (!slice->diversity()) {
            return slice;
        }
        if (!indexedDiversitySlice
            || (slice->diversityIndex() >= 0
                && (indexedDiversitySlice->diversityIndex() < 0
                    || slice->diversityIndex()
                           < indexedDiversitySlice->diversityIndex()))) {
            indexedDiversitySlice = slice;
        }
    }

    return indexedDiversitySlice ? indexedDiversitySlice : candidates.first();
}

QString MainWindow::kiwiSdrProfileForPan(const QString& panId) const
{
    SliceModel* displaySlice = kiwiSdrDisplaySliceForPan(panId);
    if (!displaySlice || !m_kiwiSdrManager) {
        return QString();
    }
    return m_kiwiSdrManager->assignedProfileForSlice(displaySlice->sliceId());
}

bool MainWindow::kiwiSdrPanDisplaysKiwi(const QString& panId) const
{
    const QString trimmedPanId = panId.trimmed();
    return !trimmedPanId.isEmpty()
        && !kiwiSdrProfileForPan(trimmedPanId).isEmpty()
        && !m_kiwiSdrFlexDisplayPans.contains(trimmedPanId);
}

void MainWindow::setKiwiSdrPanDisplaySource(const QString& panId, bool kiwi)
{
    const QString trimmedPanId = panId.trimmed();
    if (trimmedPanId.isEmpty()) {
        return;
    }

    if (kiwiSdrProfileForPan(trimmedPanId).isEmpty() || kiwi) {
        m_kiwiSdrFlexDisplayPans.remove(trimmedPanId);
    } else {
        m_kiwiSdrFlexDisplayPans.insert(trimmedPanId);
    }

    syncKiwiSdrPanadapterUiState(trimmedPanId);
    syncKiwiSdrAppletWaterfallState();
}

void MainWindow::clearKiwiSdrPanDisplaySourceOverride(const QString& panId)
{
    const QString trimmedPanId = panId.trimmed();
    if (trimmedPanId.isEmpty()) {
        return;
    }

    m_kiwiSdrFlexDisplayPans.remove(trimmedPanId);
}

void MainWindow::clearKiwiSdrPanDisplaySourceOverrides()
{
    m_kiwiSdrFlexDisplayPans.clear();
}

QString MainWindow::kiwiSdrOverlayProfileForPan(const QString& panId) const
{
    if (!m_kiwiSdrManager || panId.isEmpty()) {
        return QString();
    }

    const QString displayProfileId = kiwiSdrProfileForPan(panId);
    QSet<QString> visitedProfiles;
    QString connectingProfileId;
    QString disconnectedProfileId;

    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }

        const QString profileId =
            m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
        if (profileId.isEmpty() || profileId == displayProfileId
            || visitedProfiles.contains(profileId)) {
            continue;
        }
        visitedProfiles.insert(profileId);

        switch (m_kiwiSdrManager->state(profileId)) {
        case KiwiSdrClient::State::Error:
        case KiwiSdrClient::State::Busy:
        case KiwiSdrClient::State::CampDisconnected:
            return profileId;
        case KiwiSdrClient::State::Connecting:
        case KiwiSdrClient::State::Waiting:
            if (connectingProfileId.isEmpty()) {
                connectingProfileId = profileId;
            }
            break;
        case KiwiSdrClient::State::Disconnected:
            if (disconnectedProfileId.isEmpty()) {
                disconnectedProfileId = profileId;
            }
            break;
        case KiwiSdrClient::State::Connected:
        case KiwiSdrClient::State::Camping:
            break;
        }
    }

    return !connectingProfileId.isEmpty() ? connectingProfileId
                                          : disconnectedProfileId;
}

void MainWindow::syncKiwiSdrPanadapterTxInhibit(const QString& panId,
                                                const QString& profileId)
{
    m_radioModel.setPanTransmitInhibited(
        panId,
        !profileId.isEmpty(),
        tr("Transmit is disabled because this panadapter is displaying a KiwiSDR receiver."));
}

void MainWindow::syncKiwiSdrDiversityEscControls()
{
    QSet<int> blockedSlices;
    if (m_kiwiSdrManager) {
        for (SliceModel* slice : m_radioModel.slices()) {
            if (!slice || !slice->diversity()
                || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
                continue;
            }

            QVector<SliceModel*> group;
            bool groupHasKiwi = false;
            for (SliceModel* other : m_radioModel.slices()) {
                if (!other || !other->diversity()
                    || !m_radioModel.sliceMayBelongToUs(other->sliceId())) {
                    continue;
                }

                const bool samePan =
                    !slice->panId().isEmpty()
                    && slice->panId() == other->panId();
                const bool parentChildPair =
                    (slice->isDiversityParent() && other->isDiversityChild())
                    || (slice->isDiversityChild() && other->isDiversityParent());
                if (slice == other || samePan || parentChildPair) {
                    group.append(other);
                    groupHasKiwi = groupHasKiwi
                        || !m_kiwiSdrManager
                                ->assignedProfileForSlice(other->sliceId())
                                .isEmpty();
                }
            }

            if (!groupHasKiwi) {
                continue;
            }

            for (SliceModel* member : group) {
                blockedSlices.insert(member->sliceId());
            }
        }
    }

    // Disable Flex ESC before hiding the controls. Kiwi receive paths are not
    // phase coherent with Flex RF, so an invisible enabled combiner would be
    // misleading and could keep stale RF phase/gain state active.
    for (SliceModel* slice : m_radioModel.slices()) {
        if (slice && blockedSlices.contains(slice->sliceId())
            && slice->isDiversityParent() && slice->escEnabled()) {
            slice->setEscEnabled(false);
        }
    }

    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice) {
            continue;
        }
        if (SpectrumWidget* sw = spectrumForSlice(slice)) {
            if (VfoWidget* vfo = sw->vfoWidget(slice->sliceId())) {
                vfo->setEscControlsAvailable(
                    !blockedSlices.contains(slice->sliceId()));
            }
        }
    }
}

void MainWindow::syncKiwiSdrPanadapterUiState(const QString& panId)
{
    if (!m_panStack || panId.isEmpty()) {
        return;
    }

    const QString profileId = kiwiSdrProfileForPan(panId);
    const bool hasKiwiSource = !profileId.isEmpty();
    const bool displayKiwi = kiwiSdrPanDisplaysKiwi(panId);
    syncKiwiSdrPanadapterTxInhibit(
        panId, displayKiwi ? profileId : QString());

    SpectrumWidget* spectrum = m_panStack->spectrum(panId);
    if (!spectrum) {
        return;
    }

    SpectrumOverlayMenu* menu = spectrum->overlayMenu();
    auto syncFlexDisplaySettings = [spectrum, menu]() {
        if (!menu) {
            return;
        }
        menu->syncDisplaySettings(
            spectrum->fftAverage(), spectrum->fftFps(),
            static_cast<int>(spectrum->fftFillAlpha() * 100.0f),
            spectrum->fftWeightedAvg(), spectrum->fftFillColor(),
            spectrum->wfColorGain(), spectrum->wfBlackLevel(),
            spectrum->wfAutoBlack(), spectrum->wfAutoBlackOffset(),
            spectrum->wfLineDuration(), spectrum->noiseFloorPosition(),
            spectrum->noiseFloorEnabled(), spectrum->fftHeatMap(),
            spectrum->wfColorScheme(), spectrum->showGrid(),
            spectrum->fftLineWidth(), spectrum->wfAutoBlackRadioSide(),
            spectrum->spectrumRenderMode(), spectrum->dssFloorDepth(),
            spectrum->dssGain(), spectrum->fftLineColor());
    };

    spectrum->setKiwiSdrDisplaySourceKiwi(displayKiwi);
    spectrum->setKiwiSdrDisplaySourceControlVisible(hasKiwiSource);

    if (profileId.isEmpty()) {
        m_kiwiSdrFlexDisplayPans.remove(panId);
        spectrum->setBandwidthLimits(m_radioModel.minPanBandwidthMhz(),
                                     m_radioModel.maxPanBandwidthMhz());
        const QString overlayProfileId = kiwiSdrOverlayProfileForPan(panId);
        if (!overlayProfileId.isEmpty()) {
            spectrum->setKiwiSdrConnectionOverlay(
                true,
                kiwiConnectionOverlayDetail(
                    m_kiwiSdrManager->state(overlayProfileId),
                    m_kiwiSdrManager->stateDetail(overlayProfileId)),
                tr("Not connected to KiwiSDR"));
        } else {
            spectrum->setKiwiSdrConnectionOverlay(false);
        }
        setKiwiSdrWaterfallActive(m_radioModel, panId, spectrum, false);
        spectrum->setKiwiSdrWaterfallAvailable(false);
        spectrum->setKiwiSdrWaterfallProfile(QString());
        syncFlexDisplaySettings();
        recenterCenterLockForPan(panId);
        return;
    }

    if (!displayKiwi) {
        spectrum->setBandwidthLimits(m_radioModel.minPanBandwidthMhz(),
                                     m_radioModel.maxPanBandwidthMhz());
        spectrum->setKiwiSdrConnectionOverlay(false);
        setKiwiSdrWaterfallActive(m_radioModel, panId, spectrum, false);
        spectrum->setKiwiSdrWaterfallAvailable(
            m_kiwiSdrManager->waterfallAvailable(profileId));
        spectrum->setKiwiSdrWaterfallProfile(profileId);
        syncFlexDisplaySettings();
        recenterCenterLockForPan(panId);
        return;
    }

    spectrum->setBandwidthLimits(
        m_radioModel.minPanBandwidthMhz(),
        std::max(m_radioModel.maxPanBandwidthMhz(),
                 kKiwiSdrWaterfallFullBandwidthMhz));

    const KiwiSdrAntennaProfile profile = m_kiwiSdrManager->profile(profileId);
    const KiwiSdrClient::State state = m_kiwiSdrManager->state(profileId);
    const bool kiwiWaterfallChannelAvailable =
        m_kiwiSdrManager->waterfallAvailable(profileId);
    const bool kiwiWaterfallActive =
        KiwiSdrClient::stateAllowsReceiverControl(state)
        && kiwiWaterfallChannelAvailable;
    spectrum->setKiwiSdrWaterfallAvailable(kiwiWaterfallChannelAvailable);
    spectrum->setKiwiSdrWaterfallProfile(profileId);
    spectrum->setKiwiSdrWaterfallActive(kiwiWaterfallActive);
    const KiwiSdrWaterfallDisplayRange displayRange =
        m_kiwiSdrManager->waterfallDisplayRange(profileId);
    if (displayRange.valid) {
        spectrum->setKiwiSdrWaterfallDisplayRange(
            displayRange.minDbm,
            displayRange.maxDbm,
            displayRange.autoRange);
    } else {
        spectrum->setKiwiSdrWaterfallDisplayRange(
            static_cast<float>(profile.waterfallMinDbm),
            static_cast<float>(profile.waterfallMaxDbm),
            profile.waterfallAutoScale);
    }
    const QString overlayDetail =
        !kiwiWaterfallChannelAvailable
            ? m_kiwiSdrManager->waterfallDetail(profileId)
            : kiwiConnectionOverlayDetail(
                  state, m_kiwiSdrManager->stateDetail(profileId));
    QString overlayTitle;
    if (!kiwiWaterfallChannelAvailable) {
        if (state == KiwiSdrClient::State::Waiting) {
            overlayTitle = tr("Waiting for KiwiSDR receiver slot");
        } else if (state == KiwiSdrClient::State::Camping) {
            overlayTitle = tr("KiwiSDR monitor session");
        } else {
            overlayTitle = tr("KiwiSDR waterfall unavailable");
        }
    }
    spectrum->setKiwiSdrConnectionOverlay(
        state != KiwiSdrClient::State::Connected
            || !kiwiWaterfallChannelAvailable,
        overlayDetail,
        overlayTitle);
    if (menu) {
        if (displayRange.valid) {
            menu->syncKiwiWaterfallSettings(
                static_cast<int>(std::lround(displayRange.minDbm)),
                static_cast<int>(std::lround(displayRange.maxDbm)),
                profile.waterfallAutoScale || displayRange.autoRange,
                profile.waterfallRate);
        } else {
            menu->syncKiwiWaterfallSettings(profile.waterfallMinDbm,
                                            profile.waterfallMaxDbm,
                                            profile.waterfallAutoScale,
                                            profile.waterfallRate);
        }
    }
    recenterCenterLockForPan(panId);
}

void MainWindow::syncKiwiSdrPanadapterUiStates()
{
    if (!m_panStack) {
        return;
    }
    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet) {
            continue;
        }
        syncKiwiSdrPanadapterUiState(applet->panId());
    }
}

void MainWindow::scheduleKiwiSdrUiSync(int flags)
{
    if (flags == 0) {
        return;
    }

    m_kiwiSdrUiSyncFlags |= flags;
    if (m_kiwiSdrUiSyncPending) {
        return;
    }

    m_kiwiSdrUiSyncPending = true;
    QTimer::singleShot(0, this, [this]() {
        const int flags = m_kiwiSdrUiSyncFlags;
        m_kiwiSdrUiSyncFlags = 0;
        m_kiwiSdrUiSyncPending = false;

        if ((flags & KiwiSdrUiSyncAppletReceivers) != 0) {
            refreshKiwiSdrAppletReceivers();
        }
        if ((flags & KiwiSdrUiSyncDiversityEsc) != 0) {
            syncKiwiSdrDiversityEscControls();
        }
        const bool refreshedWaterfall =
            (flags & KiwiSdrUiSyncWaterfallAvailability) != 0;
        if (refreshedWaterfall) {
            refreshKiwiSdrWaterfallAvailability();
        }
        if (!refreshedWaterfall
            && (flags & KiwiSdrUiSyncPanadapterStates) != 0) {
            syncKiwiSdrPanadapterUiStates();
        }
    });
}

void MainWindow::updateKiwiSdrVirtualAudioControlsForSlice(SliceModel* slice)
{
    if (!m_kiwiSdrManager || !m_audio || !slice) {
        return;
    }

    const QString profileId =
        m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
    if (profileId.isEmpty()) {
        return;
    }

    const float gainPercent = slice->audioGain();
    const bool muted = slice->audioMute();
    const int pan = slice->audioPan();
    QMetaObject::invokeMethod(m_audio,
                              [audio = m_audio, profileId, gainPercent, muted, pan]() {
        audio->setKiwiSdrAudioSourceEnabled(profileId, true);
        audio->setKiwiSdrAudioSourceGain(profileId, gainPercent);
        audio->setKiwiSdrAudioSourceMuted(profileId, muted);
        audio->setKiwiSdrAudioSourcePan(profileId, pan);
    }, Qt::QueuedConnection);
}

void MainWindow::updateKiwiSdrVirtualReceiverControlsForSlice(SliceModel* slice)
{
    if (!m_kiwiSdrManager || !slice
        || !slice->externalReceiveReplacementActive()) {
        return;
    }

    const QString profileId =
        m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
    if (profileId.isEmpty()) {
        return;
    }

    int autoSqlMarginDb = -1;
    if (slice->externalReceiveAutoSquelchOn()) {
        autoSqlMarginDb = std::clamp(
            AppSettings::instance().value("AutoSqlMarginDb", "10").toInt(),
            5, 20);
    }

    m_kiwiSdrManager->setReceiverControlsForSlice(
        slice->sliceId(),
        kiwiSdrReceiverControlsForSlice(*slice, autoSqlMarginDb));
}

bool MainWindow::applyKiwiSdrSliceMute()
{
    SliceModel* target = kiwiSdrAudioTargetSlice();
    if (!target) {
        restoreKiwiSdrSliceMute();
        return false;
    }

    if (m_kiwiSdrAudioSliceId == target->sliceId()) {
        return true;
    }

    restoreKiwiSdrSliceMute();

    m_kiwiSdrAudioSliceId = target->sliceId();
    m_kiwiSdrAudioPreviousMute = target->audioMute();
    m_kiwiSdrAudioMuteApplied = !target->audioMute();
    m_kiwiSdrAudioMuteConnection =
        connect(target, &SliceModel::audioMuteChanged,
                this, [this, sliceId = target->sliceId()](bool muted) {
                    if (m_kiwiSdrAudioMuteChanging
                        || m_kiwiSdrAudioSliceId != sliceId) {
                        return;
                    }

                    if (m_kiwiSdrAudioMuteApplied && !muted) {
                        m_kiwiSdrAudioMuteApplied = false;
                    }
                });

    if (m_kiwiSdrAudioMuteApplied) {
        m_kiwiSdrAudioMuteChanging = true;
        target->setAudioMute(true);
        m_kiwiSdrAudioMuteChanging = false;
    }

    return true;
}

void MainWindow::restoreKiwiSdrSliceMute()
{
    const int sliceId = m_kiwiSdrAudioSliceId;
    const bool previousMute = m_kiwiSdrAudioPreviousMute;
    const bool muteApplied = m_kiwiSdrAudioMuteApplied;

    if (m_kiwiSdrAudioMuteConnection) {
        QObject::disconnect(m_kiwiSdrAudioMuteConnection);
        m_kiwiSdrAudioMuteConnection = {};
    }

    m_kiwiSdrAudioSliceId = -1;
    m_kiwiSdrAudioPreviousMute = false;
    m_kiwiSdrAudioMuteApplied = false;
    m_kiwiSdrAudioMuteChanging = false;

    if (sliceId < 0 || !muteApplied) {
        return;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || slice->audioMute() == previousMute) {
        return;
    }

    slice->setAudioMute(previousMute);
}

bool MainWindow::setKiwiSdrAudioRouting(bool active)
{
    bool routed = active;
    if (active) {
        routed = applyKiwiSdrSliceMute();
    } else {
        restoreKiwiSdrSliceMute();
    }

    if (!routed) {
        restoreKiwiSdrSliceMute();
    }

    if (m_audio) {
        QMetaObject::invokeMethod(m_audio, [audio = m_audio, routed]() {
            audio->setKiwiSdrAudioEnabled(routed);
        }, Qt::QueuedConnection);
    }

    return routed;
}

bool MainWindow::kiwiSdrTransmitMuteRequired() const
{
    if (m_radioModel.fullDuplexEnabled()) {
        return false;
    }

    const TransmitModel& tx = m_radioModel.transmitModel();
    bool txActive = tx.isTransmitting() || tx.isTuning()
        || m_radioModel.isRadioTransmitting();
#ifdef HAVE_RADE
    txActive = txActive || m_radeEooPending;
#endif
    return txActive;
}

void MainWindow::syncKiwiSdrTransmitMute()
{
    if (!m_audio) {
        return;
    }

    const bool muted = kiwiSdrTransmitMuteRequired();
    if (m_kiwiSdrAudioTransmitMuted == muted) {
        return;
    }

    m_kiwiSdrAudioTransmitMuted = muted;
    QMetaObject::invokeMethod(m_audio, [audio = m_audio, muted]() {
        audio->setKiwiSdrAudioTransmitMuted(muted);
    }, Qt::QueuedConnection);
}

void MainWindow::wireKiwiSdr()
{
    if (!m_appletPanel || !m_appletPanel->kiwiSdrApplet()) {
        return;
    }

    if (m_kiwiSdrManager) {
        m_kiwiSdrManager->setOperatorCallsign(m_radioModel.callsign());
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileNeedsInitialTracking,
                this, [this](const QString& profileId) {
            if (!m_kiwiSdrManager || profileId.isEmpty()) {
                return;
            }

            SliceModel* slice = kiwiSdrAudioTargetSlice();
            if (!slice) {
                return;
            }

            SpectrumWidget* spectrum = spectrumForSlice(slice);
            m_kiwiSdrManager->primeProfileTracking(
                profileId, slice->sliceId(), slice->frequency(), slice->mode(),
                slice->filterLow(), slice->filterHigh(), slice->panId(),
                spectrum ? spectrum->centerMhz() : slice->frequency(),
                spectrum ? spectrum->bandwidthMhz() : 0.2,
                spectrum ? spectrum->wfLineDuration() : 100,
                BandSettings::bandForFrequency(slice->frequency()));
        });
        if (m_audio) {
            connect(m_kiwiSdrManager, &KiwiSdrManager::decodedAudioReady,
                    m_audio, [audio = m_audio](const QString& id,
                                                const QByteArray& pcm) {
                audio->feedKiwiSdrAudioData(id, pcm);
            }, Qt::QueuedConnection);
            connect(m_kiwiSdrManager, &KiwiSdrManager::audioSourceEnabledChanged,
                    m_audio, [audio = m_audio](const QString& id, bool enabled) {
                audio->setKiwiSdrAudioSourceEnabled(id, enabled);
            }, Qt::QueuedConnection);
            connect(m_kiwiSdrManager, &KiwiSdrManager::audioSourceRemoved,
                    m_audio, [audio = m_audio](const QString& id) {
                audio->removeKiwiSdrAudioSource(id);
            }, Qt::QueuedConnection);
        }
        connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
                this, [this](bool) { syncKiwiSdrTransmitMute(); });
        connect(&m_radioModel.transmitModel(), &TransmitModel::tuneChanged,
                this, [this](bool) { syncKiwiSdrTransmitMute(); });
        connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
                this, [this](bool) { syncKiwiSdrTransmitMute(); });
        connect(&m_radioModel, &RadioModel::infoChanged,
                this, [this]() { syncKiwiSdrTransmitMute(); });
        connect(m_kiwiSdrManager, &KiwiSdrManager::waterfallRowReady,
                this, [this](const QString& profileId, const QString&,
                             const QVector<float>& binsDbm,
                             double lowFreqMhz, double highFreqMhz,
                             quint32 timecode) {
            if (!m_panStack) {
                return;
            }
            if (profileId.isEmpty() || !m_kiwiSdrManager) {
                return;
            }

            const int sliceId =
                m_kiwiSdrManager->assignedSliceForProfile(profileId);
            SliceModel* slice = m_radioModel.slice(sliceId);
            if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)) {
                return;
            }

            if (kiwiSdrProfileForPan(slice->panId()) != profileId) {
                return;
            }

            deferReceivePresentation(
                ReceivePresentationSource::KiwiSdr,
                ReceivePresentationSurface::Waterfall,
                [this, profileId, sliceId, binsDbm, lowFreqMhz, highFreqMhz,
                 timecode]() {
                    if (!m_kiwiSdrManager
                        || m_kiwiSdrManager->assignedProfileForSlice(sliceId)
                            != profileId) {
                        return;
                    }
                    SliceModel* delayedSlice = m_radioModel.slice(sliceId);
                    if (!delayedSlice
                        || !m_radioModel.sliceMayBelongToUs(sliceId)
                        || kiwiSdrProfileForPan(delayedSlice->panId())
                            != profileId) {
                        return;
                    }
                    if (SpectrumWidget* sw = spectrumForSlice(delayedSlice)) {
                        sw->setKiwiSdrWaterfallProfile(profileId);
                        sw->updateKiwiSdrWaterfallRow(binsDbm, lowFreqMhz,
                                                      highFreqMhz, timecode);
                    }
                },
                profileId);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::waterfallDisplayRangeChanged,
                this, [this](const QString& profileId, float minDbm,
                             float maxDbm, bool autoRange) {
            if (profileId.isEmpty() || !m_kiwiSdrManager || !m_panStack) {
                return;
            }

            // Resolve the target pan(s) the same way the display path does
            // (kiwiSdrProfileForPan), instead of via assignedSliceForProfile:
            // the profile→slice assignment can be absent, or point at a
            // different slice than the one a pan is actually displaying
            // (diversity / active-slice resolution, or the owned-but-unassigned
            // tracking fallback), so the assigned-slice path could drop the
            // computed range. Iterating panes also updates every pan when the
            // same profile is shown on more than one. (#4069 review)
            const KiwiSdrAntennaProfile profile =
                m_kiwiSdrManager->profile(profileId);
            const int minInt = static_cast<int>(std::lround(minDbm));
            const int maxInt = static_cast<int>(std::lround(maxDbm));
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                if (!applet
                    || kiwiSdrProfileForPan(applet->panId()) != profileId) {
                    continue;
                }
                SpectrumWidget* sw = m_panStack->spectrum(applet->panId());
                if (!sw) {
                    continue;
                }
                sw->setKiwiSdrWaterfallProfile(profileId);
                sw->setKiwiSdrWaterfallDisplayRange(minDbm, maxDbm, autoRange);
                if (SpectrumOverlayMenu* menu = sw->overlayMenu()) {
                    menu->syncKiwiWaterfallSettings(
                        minInt, maxInt,
                        profile.waterfallAutoScale || autoRange,
                        profile.waterfallRate);
                }
            }
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::meterReadingReady,
                this, [this](const QString& profileId,
                             const KiwiSdrProtocol::MeterReading& reading) {
            const auto applyMeterReading =
                [this](const QString& id,
                       const KiwiSdrProtocol::MeterReading& meter,
                       bool requireConnected) {
                if (!m_kiwiSdrManager || id.isEmpty()) {
                    return;
                }
                const int sliceId =
                    m_kiwiSdrManager->assignedSliceForProfile(id);
                if (sliceId < 0
                    || m_kiwiSdrManager->assignedProfileForSlice(sliceId)
                        != id) {
                    return;
                }
                if (requireConnected
                    && !KiwiSdrClient::stateHasReceiveAudio(
                        m_kiwiSdrManager->state(id))) {
                    return;
                }
                SliceModel* slice = m_radioModel.slice(sliceId);
                if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)) {
                    return;
                }
                if (SpectrumWidget* sw = spectrumForSlice(slice)) {
                    if (meter.valid && meter.hasDbm) {
                        sw->setKiwiSdrSquelchMeterDbm(
                            meter.dbm,
                            meter.squelchStateKnown && meter.squelched);
                    }
                    if (VfoWidget* vfo = sw->vfoWidget(sliceId)) {
                        vfo->setReceiveMeterReading(meter);
                    }
                }
                if (sliceId == m_activeSliceId && m_appletPanel
                    && m_appletPanel->sMeterWidget()) {
                    m_appletPanel->sMeterWidget()->setReceiveMeterReading(meter);
                }
            };

            if (!reading.valid || !reading.hasDbm) {
                applyMeterReading(profileId, reading, false);
                return;
            }

            deferReceivePresentation(
                ReceivePresentationSource::KiwiSdr,
                ReceivePresentationSurface::Meter,
                [profileId, reading, applyMeterReading]() {
                    applyMeterReading(profileId, reading, true);
                },
                profileId);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileTelemetryChanged,
                this, [this](const QString&,
                             const KiwiSdrReceiverTelemetry&) {
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncAppletReceivers);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::sliceAssignmentChanged,
                this, [this](int sliceId, const QString& profileId) {
            const QString panId = m_radioModel.slice(sliceId)
                ? m_radioModel.slice(sliceId)->panId()
                : QString();
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncAppletReceivers
                                  | KiwiSdrUiSyncDiversityEsc);
            if (m_appletPanel) {
                m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
            }
            const ReceivePresentationSettings syncSettings =
                m_receivePresentationSync.settings();
            if (syncSettings.enabled
                && syncSettings.mode == ReceiveSyncMode::AutoAssist) {
                const QString previousSyncProfile = m_receiveSyncKiwiProfileId;
                const QString currentSyncProfile = receiveSyncKiwiProfileId();
                if (previousSyncProfile != currentSyncProfile) {
                    if (currentSyncProfile.isEmpty()
                        && !previousSyncProfile.isEmpty()) {
                        if (!m_receiveSyncTargetUnavailable) {
                            holdReceivePresentationAutoAssistLock(false);
                            clearReceivePresentationVisualQueueForSource(
                                ReceivePresentationSource::KiwiSdr,
                                previousSyncProfile);
                            resetReceivePresentationAudioBuffersForKiwiSource(
                                previousSyncProfile);
                            m_receiveSyncTargetUnavailable = true;
                            syncReceivePresentationDelaysToAudioEngine();
                        }
                    } else {
                        resetReceivePresentationAutoAssistState(true, false);
                        if (!previousSyncProfile.isEmpty()) {
                            clearReceivePresentationVisualQueueForSource(
                                ReceivePresentationSource::KiwiSdr,
                                previousSyncProfile);
                            resetReceivePresentationAudioBuffersForKiwiSource(
                                previousSyncProfile);
                        }
                        m_receiveSyncKiwiProfileId = currentSyncProfile;
                        m_receiveSyncTargetUnavailable = false;
                        syncReceivePresentationDelaysToAudioEngine();
                    }
                }
            }
            if (!profileId.isEmpty()) {
                syncKiwiSdrPanadapterUiState(panId);
                return;
            }
            if (SliceModel* slice = m_radioModel.slice(sliceId)) {
                const bool restoreMute =
                    m_kiwiSdrVirtualPreviousMute.contains(sliceId)
                        ? m_kiwiSdrVirtualPreviousMute.take(sliceId)
                        : slice->flexAudioMute();
                slice->setExternalReceiveAudioReplacementMute(false, restoreMute);
                if (m_appletPanel) {
                    m_appletPanel->updateSliceButtons(
                        m_radioModel.slices(), m_activeSliceId);
                }
                if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
                    setKiwiSdrWaterfallActive(
                        m_radioModel, slice->panId(), spectrum, false);
                    spectrum->setKiwiSdrConnectionOverlay(false);
                    spectrum->setKiwiSdrWaterfallProfile(QString());
                }
            }
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncWaterfallAvailability
                                  | KiwiSdrUiSyncDiversityEsc);
            syncKiwiSdrPanadapterUiState(panId);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileStateChanged,
                this, [this](const QString& profileId, KiwiSdrClient::State state,
                             const QString&) {
            QString panId;
            if (KiwiSdrClient::stateHasReceiveAudio(state)) {
                const int sliceId =
                    m_kiwiSdrManager
                        ? m_kiwiSdrManager->assignedSliceForProfile(profileId)
                        : -1;
                if (SliceModel* slice = m_radioModel.slice(sliceId)) {
                    panId = slice->panId();
                    updateKiwiSdrVirtualTrackingForSlice(slice);
                    updateKiwiSdrVirtualAudioControlsForSlice(slice);
                    updateKiwiSdrVirtualReceiverControlsForSlice(slice);
                }
            } else if (m_kiwiSdrManager) {
                const int sliceId =
                    m_kiwiSdrManager->assignedSliceForProfile(profileId);
                if (SliceModel* slice = m_radioModel.slice(sliceId)) {
                    panId = slice->panId();
                }
            }
            const ReceivePresentationSettings syncSettings =
                m_receivePresentationSync.settings();
            if (syncSettings.enabled
                && syncSettings.mode == ReceiveSyncMode::AutoAssist) {
                const QString previousSyncProfile = m_receiveSyncKiwiProfileId;
                const QString currentSyncProfile = receiveSyncKiwiProfileId();
                if (previousSyncProfile != currentSyncProfile) {
                    if (currentSyncProfile.isEmpty()
                        && !previousSyncProfile.isEmpty()) {
                        holdReceivePresentationAutoAssistLock(false);
                        clearReceivePresentationVisualQueueForSource(
                            ReceivePresentationSource::KiwiSdr,
                            previousSyncProfile);
                        resetReceivePresentationAudioBuffersForKiwiSource(
                            previousSyncProfile);
                        m_receiveSyncTargetUnavailable = true;
                    } else {
                        resetReceivePresentationAutoAssistState(true, false);
                        if (!previousSyncProfile.isEmpty()) {
                            clearReceivePresentationVisualQueueForSource(
                                ReceivePresentationSource::KiwiSdr,
                                previousSyncProfile);
                            resetReceivePresentationAudioBuffersForKiwiSource(
                                previousSyncProfile);
                        }
                        m_receiveSyncKiwiProfileId = currentSyncProfile;
                        m_receiveSyncTargetUnavailable = false;
                    }
                }
                syncReceivePresentationDelaysToAudioEngine();
            }
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncWaterfallAvailability
                                  | KiwiSdrUiSyncDiversityEsc
                                  | KiwiSdrUiSyncAppletReceivers);
            if (!panId.isEmpty()) {
                syncKiwiSdrPanadapterUiState(panId);
            }
        });
        connect(m_kiwiSdrManager,
                &KiwiSdrManager::profileWaterfallAvailabilityChanged,
                this, [this](const QString&, bool, const QString&) {
            if (!m_kiwiSdrManager) {
                return;
            }

            scheduleKiwiSdrUiSync(KiwiSdrUiSyncAppletReceivers
                                  | KiwiSdrUiSyncPanadapterStates);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileStreamReset,
                this, [this](const QString& profileId) {
            if (!m_panStack || profileId.isEmpty()) {
                return;
            }
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                if (applet && applet->spectrumWidget()) {
                    applet->spectrumWidget()
                        ->clearKiwiSdrWaterfallRowsForProfile(profileId);
                }
            }
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncPanadapterStates);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profilesChanged,
                this, [this] {
            scheduleKiwiSdrUiSync(KiwiSdrUiSyncWaterfallAvailability
                                  | KiwiSdrUiSyncAppletReceivers
                                  | KiwiSdrUiSyncPanadapterStates);
        });
    }
    connect(&m_radioModel, &RadioModel::infoChanged,
            this, [this]() {
        if (m_kiwiSdrManager) {
            m_kiwiSdrManager->setOperatorCallsign(m_radioModel.callsign());
        }
    });
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();
    syncKiwiSdrTransmitMute();

    if (m_kiwiSdrManager) {
        QTimer::singleShot(0, m_kiwiSdrManager, &KiwiSdrManager::startAutoConnect);
    }
    scheduleKiwiSdrUiSync(KiwiSdrUiSyncPanadapterStates
                          | KiwiSdrUiSyncDiversityEsc);
}

void MainWindow::refreshKiwiSdrAppletReceivers()
{
    if (!m_appletPanel || !m_appletPanel->kiwiSdrApplet()) {
        return;
    }

    QVector<KiwiSdrReceiverStatus> receivers;
    if (m_kiwiSdrManager) {
        for (const KiwiSdrAntennaProfile& profile : m_kiwiSdrManager->profiles()) {
            KiwiSdrReceiverStatus receiver;
            receiver.id = profile.id;
            receiver.name = m_kiwiSdrManager->displayName(profile.id);
            receiver.state = m_kiwiSdrManager->state(profile.id);
            receiver.detail = m_kiwiSdrManager->stateDetail(profile.id);
            if (receiver.state == KiwiSdrClient::State::Connected
                && !m_kiwiSdrManager->waterfallAvailable(profile.id)) {
                receiver.detail = m_kiwiSdrManager->waterfallDetail(profile.id);
            }
            receiver.metadataSummary = kiwiReceiverMetadataSummary(
                m_kiwiSdrManager->receiverMetadata(profile.id));
            receiver.protocolSummary = kiwiProtocolSummary(
                m_kiwiSdrManager->protocolState(profile.id));
            const int sliceId = m_kiwiSdrManager->assignedSliceForProfile(profile.id);
            receiver.assignedSlice = m_radioModel.slice(sliceId);
            receivers.append(receiver);
        }
    }

    m_appletPanel->kiwiSdrApplet()->setReceivers(receivers);
}

void MainWindow::refreshKiwiSdrSlices()
{
    refreshKiwiSdrAppletReceivers();
}

void MainWindow::refreshKiwiSdrWaterfallAvailability()
{
    if (!m_panStack) {
        return;
    }

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet || !applet->spectrumWidget()) {
            continue;
        }

        SpectrumWidget* spectrum = applet->spectrumWidget();
        bool available = false;
        if (m_kiwiSdrManager) {
            for (SliceModel* slice : m_radioModel.slices()) {
                if (!slice || slice->panId() != applet->panId()
                    || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
                    continue;
                }
                const QString profileId =
                    m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
                if (!profileId.isEmpty()) {
                    available = true;
                    break;
                }
            }
        }
        spectrum->setKiwiSdrWaterfallAvailable(available);
        syncKiwiSdrPanadapterUiState(applet->panId());
    }
    syncKiwiSdrAppletWaterfallState();
}

void MainWindow::syncKiwiSdrAppletWaterfallState()
{
    refreshKiwiSdrAppletReceivers();
}

} // namespace AetherSDR
