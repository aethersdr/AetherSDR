#include "MainWindow.h"

#include "AppletPanel.h"
#include "KiwiSdrApplet.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "SMeterWidget.h"
#include "VfoWidget.h"
#include "core/AudioEngine.h"
#include "core/KiwiSdrClient.h"
#include "core/KiwiSdrManager.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QMetaObject>
#include <QTimer>

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

    if (!m_kiwiSdrVirtualPreviousMute.contains(sliceId)) {
        m_kiwiSdrVirtualPreviousMute.insert(sliceId, slice->flexAudioMute());
    }
    slice->setExternalReceiveAudioReplacementMute(true);

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

    if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
        spectrum->setKiwiSdrWaterfallAvailable(true);
        spectrum->setKiwiSdrWaterfallProfile(profileId);
        spectrum->setKiwiSdrWaterfallActive(true);
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
        if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
            setKiwiSdrWaterfallActive(m_radioModel, panId, spectrum, false);
            spectrum->setKiwiSdrConnectionOverlay(false);
            spectrum->setKiwiSdrWaterfallProfile(QString());
        }
    }

    if (m_kiwiSdrManager) {
        m_kiwiSdrManager->clearSliceAssignment(sliceId);
    }
    refreshKiwiSdrWaterfallAvailability();
    if (!panId.isEmpty()) {
        syncKiwiSdrPanadapterUiState(panId);
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
        if (m_kiwiSdrManager->isConnected(profileId)) {
            spectrum->setKiwiSdrWaterfallAvailable(true);
            spectrum->setKiwiSdrWaterfallActive(true);
            syncKiwiSdrAppletWaterfallState();
        }
        syncKiwiSdrPanadapterUiState(slice->panId());
    }
}

QString MainWindow::kiwiSdrProfileForPan(const QString& panId) const
{
    if (!m_kiwiSdrManager || panId.isEmpty()) {
        return QString();
    }

    if (SliceModel* slice = activeSlice()) {
        if (slice->panId() == panId
            && m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            const QString profileId =
                m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
            if (!profileId.isEmpty()) {
                return profileId;
            }
        }
    }

    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }
        const QString profileId =
            m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
        if (!profileId.isEmpty()) {
            return profileId;
        }
    }
    return QString();
}

void MainWindow::syncKiwiSdrPanadapterUiState(const QString& panId)
{
    if (!m_panStack || panId.isEmpty()) {
        return;
    }

    SpectrumWidget* spectrum = m_panStack->spectrum(panId);
    if (!spectrum) {
        return;
    }

    const QString profileId = kiwiSdrProfileForPan(panId);
    SpectrumOverlayMenu* menu = spectrum->overlayMenu();
    if (profileId.isEmpty()) {
        spectrum->setKiwiSdrConnectionOverlay(false);
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
    spectrum->setKiwiSdrWaterfallAvailable(true);
    spectrum->setKiwiSdrWaterfallProfile(profileId);
    spectrum->setKiwiSdrWaterfallActive(true);
    spectrum->setKiwiSdrWaterfallAdjustments(profile.waterfallCellDb,
                                             profile.waterfallFloorDb);
    const KiwiSdrClient::State state = m_kiwiSdrManager->state(profileId);
    spectrum->setKiwiSdrConnectionOverlay(
        state != KiwiSdrClient::State::Connected,
        kiwiConnectionOverlayDetail(
            state, m_kiwiSdrManager->stateDetail(profileId)));
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
    QMetaObject::invokeMethod(m_audio,
                              [audio = m_audio, profileId, gainPercent, muted]() {
        audio->setKiwiSdrAudioSourceGain(profileId, gainPercent);
        audio->setKiwiSdrAudioSourceMuted(profileId, muted);
    }, Qt::QueuedConnection);
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
                if (SpectrumWidget* spectrum = spectrumForSlice(slice)) {
                    setKiwiSdrWaterfallActive(
                        m_radioModel, slice->panId(), spectrum, false);
                    spectrum->setKiwiSdrConnectionOverlay(false);
                    spectrum->setKiwiSdrWaterfallProfile(QString());
                }
            }
            refreshKiwiSdrWaterfallAvailability();
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
                }
            } else if (m_kiwiSdrManager) {
                const int sliceId =
                    m_kiwiSdrManager->assignedSliceForProfile(profileId);
                if (SliceModel* slice = m_radioModel.slice(sliceId)) {
                    panId = slice->panId();
                }
            }
            refreshKiwiSdrWaterfallAvailability();
            refreshKiwiSdrAppletReceivers();
            if (!panId.isEmpty()) {
                syncKiwiSdrPanadapterUiState(panId);
            }
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
