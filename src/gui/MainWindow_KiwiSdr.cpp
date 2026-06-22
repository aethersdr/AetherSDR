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
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QMetaObject>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {
namespace {

constexpr int kKiwiSdrMeterDisplayDelayMs = 520;

const SliceModel* kiwiSliceForPan(const RadioModel& radioModel,
                                  const QString& panId,
                                  int activeSliceId)
{
    if (panId.isEmpty()) {
        return nullptr;
    }

    for (SliceModel* slice : radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }

        if (slice->sliceId() == activeSliceId) {
            return slice;
        }
    }
    return nullptr;
}

QString kiwiConnectionOverlayDetail(KiwiSdrClient::State state,
                                    const QString& detail)
{
    const QString trimmed = detail.trimmed();
    switch (state) {
    case KiwiSdrClient::State::Connecting:
        return trimmed.isEmpty() ? QStringLiteral("Connecting") : trimmed;
    case KiwiSdrClient::State::Error:
        return trimmed.isEmpty() ? QStringLiteral("Connection error") : trimmed;
    case KiwiSdrClient::State::Connected:
        return QString();
    case KiwiSdrClient::State::Disconnected:
        return trimmed.isEmpty() ? QStringLiteral("Disconnected") : trimmed;
    }
    return trimmed.isEmpty() ? QStringLiteral("Disconnected") : trimmed;
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
    if (!m_kiwiSdrManager || profileId.isEmpty()) {
        return;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)) {
        return;
    }

    // Selecting a receive source from a VFO/header menu is also selecting the
    // slice whose receive path is being changed. Keep the side RX applet bound
    // to that slice so SQL/AGC controls drive the Kiwi replacement source, not
    // whichever Flex slice happened to be active before the menu action.
    if (sliceId != m_activeSliceId) {
        setActiveSliceInternal(sliceId, false);
    }

    if (!m_kiwiSdrVirtualPreviousMute.contains(sliceId)) {
        m_kiwiSdrVirtualPreviousMute.insert(sliceId, slice->flexAudioMute());
    }
    slice->setExternalReceiveAudioReplacementMute(true);
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
        slice->filterLow(), slice->filterHigh(), slice->panId());
    updateKiwiSdrVirtualTrackingForSlice(slice);
    updateKiwiSdrVirtualAudioControlsForSlice(slice);
    updateKiwiSdrVirtualReceiverControlsForSlice(slice);
    syncActiveSliceSquelchLineToSpectrums();
    syncActiveSliceAutoSquelchToSpectrums();
    syncFlexRxPanToAudioEngine();
    syncKiwiSdrDiversityEscControls();

    if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
        if (kiwiSdrDisplaySliceForPan(slice->panId()) == slice) {
            spectrum->setKiwiSdrWaterfallAvailable(true);
            spectrum->setKiwiSdrWaterfallProfile(profileId);
            spectrum->setKiwiSdrWaterfallActive(true);
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
    refreshKiwiSdrWaterfallAvailability();
}

void MainWindow::clearKiwiSdrVirtualAntennaForSlice(int sliceId)
{
    qCInfo(lcKiwiSdr).noquote()
        << "Virtual RX antenna cleared for slice" << sliceId
        << "(Flex audio restored)";
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

    if (m_kiwiSdrManager) {
        m_kiwiSdrManager->clearSliceAssignment(sliceId);
    }
    syncFlexRxPanToAudioEngine();
    syncActiveSliceSquelchLineToSpectrums();
    syncActiveSliceAutoSquelchToSpectrums();
    syncKiwiSdrDiversityEscControls();
    refreshKiwiSdrWaterfallAvailability();
    if (!panId.isEmpty()) {
        syncKiwiSdrPanadapterUiState(panId);
    }
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

    const QString profileId =
        m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
    if (profileId.isEmpty()) {
        return;
    }

    m_kiwiSdrManager->updateSliceTracking(
        slice->sliceId(), slice->frequency(), slice->mode(),
        slice->filterLow(), slice->filterHigh(), slice->panId());
    if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
        m_kiwiSdrManager->updateWaterfallView(
            slice->sliceId(), slice->panId(), spectrum->centerMhz(),
            spectrum->bandwidthMhz(), spectrum->wfLineDuration());
        if (profileId == kiwiSdrProfileForPan(slice->panId())
            && m_kiwiSdrManager->isConnected(profileId)) {
            spectrum->setKiwiSdrWaterfallAvailable(true);
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
            return profileId;
        case KiwiSdrClient::State::Connecting:
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
    syncKiwiSdrPanadapterTxInhibit(panId, profileId);

    SpectrumWidget* spectrum = m_panStack->spectrum(panId);
    if (!spectrum) {
        return;
    }

    SpectrumOverlayMenu* menu = spectrum->overlayMenu();
    if (profileId.isEmpty()) {
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
        if (menu) {
            menu->syncDisplaySettings(
                spectrum->fftAverage(), spectrum->fftFps(),
                static_cast<int>(spectrum->fftFillAlpha() * 100.0f),
                spectrum->fftWeightedAvg(), spectrum->fftFillColor(),
                spectrum->wfColorGain(), spectrum->wfBlackLevel(),
                spectrum->wfAutoBlack(), spectrum->wfAutoBlackOffset(),
                spectrum->wfLineDuration(), spectrum->noiseFloorPosition(),
                spectrum->noiseFloorEnabled(), spectrum->fftHeatMap(),
                spectrum->wfColorScheme(), spectrum->showGrid(),
                spectrum->fftLineWidth());
        }
        return;
    }

    const KiwiSdrAntennaProfile profile = m_kiwiSdrManager->profile(profileId);
    const KiwiSdrClient::State state = m_kiwiSdrManager->state(profileId);
    const bool kiwiWaterfallChannelAvailable =
        state == KiwiSdrClient::State::Connected
            ? m_kiwiSdrManager->waterfallAvailable(profileId)
            : true;
    spectrum->setKiwiSdrWaterfallAvailable(true);
    spectrum->setKiwiSdrWaterfallProfile(profileId);
    spectrum->setKiwiSdrWaterfallActive(true);
    spectrum->setKiwiSdrWaterfallAdjustments(profile.waterfallCellDb,
                                             profile.waterfallFloorDb);
    const QString overlayDetail =
        state == KiwiSdrClient::State::Connected
            && !kiwiWaterfallChannelAvailable
            ? m_kiwiSdrManager->waterfallDetail(profileId)
            : kiwiConnectionOverlayDetail(
                  state, m_kiwiSdrManager->stateDetail(profileId));
    const QString overlayTitle =
        state == KiwiSdrClient::State::Connected
            && !kiwiWaterfallChannelAvailable
            ? tr("KiwiSDR waterfall unavailable")
            : QString();
    spectrum->setKiwiSdrConnectionOverlay(
        state != KiwiSdrClient::State::Connected
            || !kiwiWaterfallChannelAvailable,
        overlayDetail,
        overlayTitle);
    if (menu) {
        menu->syncKiwiWaterfallSettings(profile.waterfallCellDb,
                                        profile.waterfallFloorDb,
                                        profile.waterfallRate);
    }
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

void MainWindow::wireKiwiSdr()
{
    if (!m_appletPanel || !m_appletPanel->kiwiSdrApplet() || m_kiwiSdrClient) {
        return;
    }

    m_kiwiSdrClient = new KiwiSdrClient(this);
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
                spectrum ? spectrum->wfLineDuration() : 100);
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

            if (SpectrumWidget* sw = spectrumForSlice(slice)) {
                sw->setKiwiSdrWaterfallProfile(profileId);
                sw->updateKiwiSdrWaterfallRow(binsDbm, lowFreqMhz,
                                              highFreqMhz, timecode);
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
                    && m_kiwiSdrManager->state(id)
                    != KiwiSdrClient::State::Connected) {
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

            // Kiwi audio intentionally prebuffers before mixing to avoid
            // WebSocket jitter. Delay visual meter samples by the same target
            // so the S-meter follows the audio the operator is hearing.
            QTimer::singleShot(
                kKiwiSdrMeterDisplayDelayMs, this,
                [profileId, reading, applyMeterReading]() {
                    applyMeterReading(profileId, reading, true);
                });
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::sliceAssignmentChanged,
                this, [this](int sliceId, const QString& profileId) {
            const QString panId = m_radioModel.slice(sliceId)
                ? m_radioModel.slice(sliceId)->panId()
                : QString();
            refreshKiwiSdrAppletReceivers();
            syncKiwiSdrDiversityEscControls();
            if (m_appletPanel) {
                m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
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
            refreshKiwiSdrWaterfallAvailability();
            syncKiwiSdrDiversityEscControls();
            syncKiwiSdrPanadapterUiState(panId);
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profileStateChanged,
                this, [this](const QString& profileId, KiwiSdrClient::State state,
                             const QString&) {
            QString panId;
            if (state == KiwiSdrClient::State::Connected) {
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
            refreshKiwiSdrWaterfallAvailability();
            syncKiwiSdrDiversityEscControls();
            refreshKiwiSdrAppletReceivers();
            if (!panId.isEmpty()) {
                syncKiwiSdrPanadapterUiState(panId);
            }
        });
        connect(m_kiwiSdrManager,
                &KiwiSdrManager::profileWaterfallAvailabilityChanged,
                this, [this](const QString& profileId, bool, const QString&) {
            if (!m_kiwiSdrManager) {
                return;
            }

            const int sliceId =
                m_kiwiSdrManager->assignedSliceForProfile(profileId);
            if (SliceModel* slice = m_radioModel.slice(sliceId)) {
                syncKiwiSdrPanadapterUiState(slice->panId());
            }
            refreshKiwiSdrAppletReceivers();
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
            syncKiwiSdrPanadapterUiStates();
        });
        connect(m_kiwiSdrManager, &KiwiSdrManager::profilesChanged,
                this, [this] {
            refreshKiwiSdrWaterfallAvailability();
            refreshKiwiSdrAppletReceivers();
            syncKiwiSdrPanadapterUiStates();
        });
    }
    m_kiwiSdrClient->setOperatorCallsign(m_radioModel.callsign());
    connect(&m_radioModel, &RadioModel::infoChanged,
            m_kiwiSdrClient, [this]() {
                if (m_kiwiSdrClient) {
                    m_kiwiSdrClient->setOperatorCallsign(m_radioModel.callsign());
                }
                if (m_kiwiSdrManager) {
                    m_kiwiSdrManager->setOperatorCallsign(m_radioModel.callsign());
                }
            });
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();

    if (m_kiwiSdrManager) {
        QTimer::singleShot(0, m_kiwiSdrManager, &KiwiSdrManager::startAutoConnect);
    }
    syncKiwiSdrPanadapterUiStates();
    syncKiwiSdrDiversityEscControls();
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

    const bool connected = m_kiwiSdrClient && m_kiwiSdrClient->isConnected();

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet || !applet->spectrumWidget()) {
            continue;
        }

        SpectrumWidget* spectrum = applet->spectrumWidget();
        bool available = connected
            && kiwiSliceForPan(m_radioModel, applet->panId(), m_activeSliceId) != nullptr;
        if (!available && m_kiwiSdrManager) {
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

void MainWindow::syncKiwiSdrTrackingToActiveSlice()
{
    if (!m_kiwiSdrClient || !m_kiwiSdrClient->isConnected()) {
        return;
    }

    SliceModel* slice = activeSlice();
    if (!slice || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
        return;
    }

    SpectrumWidget* target = spectrumForSlice(slice);
    if (!target) {
        return;
    }

    bool kiwiWaterfallWasActive = false;
    if (m_panStack) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (!applet || !applet->spectrumWidget()) {
                continue;
            }
            SpectrumWidget* spectrum = applet->spectrumWidget();
            if (spectrum->kiwiSdrWaterfallActive()) {
                kiwiWaterfallWasActive = true;
                if (spectrum != target) {
                    setKiwiSdrWaterfallActive(
                        m_radioModel, applet->panId(), spectrum, false);
                }
            }
        }
    }

    m_kiwiSdrClient->setTrackedSlice(
        slice->sliceId(),
        slice->frequency(),
        slice->mode(),
        slice->filterLow(),
        slice->filterHigh(),
        slice->panId());
    m_kiwiSdrClient->setWaterfallLineDurationMs(target->wfLineDuration());
    m_kiwiSdrClient->setWaterfallView(
        slice->panId(), target->centerMhz(), target->bandwidthMhz());

    if (kiwiWaterfallWasActive) {
        target->setKiwiSdrWaterfallActive(true);
    }
    syncKiwiSdrAppletWaterfallState();
}

void MainWindow::setKiwiSdrWaterfallForActiveSlice(bool active)
{
    bool allowed = active && m_kiwiSdrClient && m_kiwiSdrClient->isConnected();
    SliceModel* slice = activeSlice();
    SpectrumWidget* target = slice ? spectrumForSlice(slice) : nullptr;
    if (allowed
        && (!slice || !target || !m_radioModel.sliceMayBelongToUs(slice->sliceId()))) {
        allowed = false;
    }

    if (allowed) {
        m_kiwiSdrClient->setTrackedSlice(
            slice->sliceId(),
            slice->frequency(),
            slice->mode(),
            slice->filterLow(),
            slice->filterHigh(),
            slice->panId());
        m_kiwiSdrClient->setWaterfallLineDurationMs(target->wfLineDuration());
        m_kiwiSdrClient->setWaterfallView(
            slice->panId(), target->centerMhz(), target->bandwidthMhz());

        if (m_panStack) {
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                if (!applet || applet->spectrumWidget() == target) {
                    continue;
                }
                setKiwiSdrWaterfallActive(
                    m_radioModel, applet->panId(), applet->spectrumWidget(), false);
            }
        }
    }

    if (target) {
        setKiwiSdrWaterfallActive(
            m_radioModel, slice ? slice->panId() : QString(), target, allowed);
    } else if (!allowed && m_panStack) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (!applet || !applet->spectrumWidget()) {
                continue;
            }
            setKiwiSdrWaterfallActive(
                m_radioModel, applet->panId(), applet->spectrumWidget(), false);
        }
    }
    refreshKiwiSdrWaterfallAvailability();
    syncKiwiSdrAppletWaterfallState();
}

void MainWindow::syncKiwiSdrAppletWaterfallState()
{
    refreshKiwiSdrAppletReceivers();
}

} // namespace AetherSDR
