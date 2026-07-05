// MainWindow_DigitalModes.cpp — digital-mode subsystems of MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1e). Holds the
// activation/deactivation and routing for every digital-mode surface:
//
//   • RADE digital voice: activateRADE / deactivateRADE / slice-mode watch
//   • Classic FreeDV (FDVU/FDVL): display activation + meter routing +
//     the FreeDV Reporter window and spot reporting
//   • DAX: startDax / stopDax / per-slice channel wiring
//   • AX.25 / AetherModem: decode dialog + KISS TNC startup
//   • RTTY: decoder output routing
//   • WFM software FM demod: activateWFM / deactivateWFM with NCO-Doppler
//     tracking + reflectWfmButtons UI state sync (DAX IQ input, #3407)
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "Ax25HfPacketDecodeDialog.h"
#include "PskReporterMapDialog.h"
#include "DaxApplet.h"
#include "PhoneCwApplet.h"
#include "RxApplet.h"
#include "VfoWidget.h"
#include "DaxIqApplet.h"
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "WfmDeviceDialog.h"
#ifdef HAVE_RADE
#include "RadeApplet.h"
#include "core/RADEEngine.h"
#endif
#if defined(Q_OS_MAC)
#include "core/VirtualAudioBridge.h"
#elif defined(HAVE_PIPEWIRE)
#include "core/PipeWireAudioBridge.h"
#endif
#include "core/AppSettings.h"
#include "core/aprs/AprsSettings.h"
#include "core/LogManager.h"
#include "core/WfmDemodulator.h"
#include "core/WfmSettings.h"
#include "models/BandPlanManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

void MainWindow::showAx25HfPacketDecodeDialog()
{
    SliceModel* slice = activeSlice();

    // Construct on first open if it didn't already come up via
    // startKissTncOnStartupIfConfigured(). Intentionally NOT using
    // showOrRaisePersistent() here because that template sets
    // WA_DeleteOnClose: closing the window would destroy the dialog and
    // along with it the KISS TCP server, dropping every connected client
    // without warning. The TNC server lifecycle is decoupled from the
    // window's open/close cycle — the dialog stays alive as long as
    // MainWindow does and is just hidden on close.
    if (!m_ax25HfPacketDecodeDialog) {
        auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, slice, this);
        dlg->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        m_ax25HfPacketDecodeDialog = dlg;
        m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
    }
    m_ax25HfPacketDecodeDialog->setAttachedSlice(slice);
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
    m_ax25HfPacketDecodeDialog->show();
    m_ax25HfPacketDecodeDialog->raise();
    m_ax25HfPacketDecodeDialog->activateWindow();
}

void MainWindow::startKissTncOnStartupIfConfigured()
{
    if (m_ax25HfPacketDecodeDialog)
        return; // already constructed (e.g. user opened the window)
    // One-shot migration from legacy flat keys (AetherModemKissTnc*) into
    // the nested-JSON blob (Constitution Principle V). Safe to call on
    // every startup — no-op once the blob exists.
    TncSettings::migrateLegacy();
    // Two launch-time services live in the AetherModem dialog: the KISS TCP
    // server and the APRS modem autostart. Either one warrants constructing
    // the dialog now.
    if (!TncSettings::startOnStartup() && !AprsSettings::modemAutostart())
        return;

    // Construct the AetherModem window hidden and persistent (no WA_DeleteOnClose)
    // so the KISS TCP server / APRS modem runs from launch and survives the
    // window being closed. The dialog's constructor auto-starts the TNC and
    // the modem per their respective settings.
    auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, activeSlice(), this);
    dlg->setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    m_ax25HfPacketDecodeDialog = dlg;
    m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
}

// External-controller methods (FlexControl, HID encoders / RC-28 / TMate 2 /
// Ulanzi / PowerMate / Shuttle, StreamDeck labels, control-devices snapshot)
// live in MainWindow_Controllers.cpp (#3351 Phase 1a).


void MainWindow::routeRttyDecoderOutput()
{
    PanadapterApplet* target = nullptr;
    if (auto* s = activeSlice(); s && m_panStack && !s->panId().isEmpty())
        target = m_panStack->panadapter(s->panId());
    if (!target) target = m_panApplet;

    if (target == m_rttyDecoderApplet) return;

    if (m_rttyDecoderApplet) {
        disconnect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                   m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        disconnect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                   m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setShiftHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                   &m_rttyDecoder, &RttyDecoder::setBaudRate);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                   &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                   &m_rttyDecoder, &RttyDecoder::stop);
    }

    m_rttyDecoderApplet = target;

    if (m_rttyDecoderApplet) {
        connect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        connect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                &m_rttyDecoder, &RttyDecoder::setShiftHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                &m_rttyDecoder, &RttyDecoder::setBaudRate);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                &m_rttyDecoder, &RttyDecoder::stop);
    }
}

void MainWindow::refreshRttyDecodeState()
{
    auto* s = activeSlice();
    // Only auto-activate for explicit RTTY mode.  DIGL is a general LSB-data
    // mode used for PSK31, FT8, SSTV, etc. — showing a Baudot decoder on
    // those signals would be confusing.  Users who do FSK on DIGL can open
    // the panel manually via the slice context menu (future work).
    const bool isRtty = s && s->mode() == "RTTY";

    if (m_rttyDecoderApplet)
        m_rttyDecoderApplet->setRttyPanelVisible(isRtty);

    if (!isRtty) {
        if (m_rttyDecoder.isRunning()) m_rttyDecoder.stop();
        return;
    }

    // Can't push params or start without the applet — the panel combos
    // hold the user's choices and we have no fallback source for them.
    if (!m_rttyDecoderApplet) return;

    const int markHz = m_rttyDecoderApplet->rttyMarkHz();
    // markHz == 0 means "Auto": follow the radio's rttyMark setting
    const int effectiveMark = (markHz > 0) ? markHz : s->rttyMark();
    m_rttyDecoder.setMarkFreqHz(effectiveMark);
    m_rttyDecoder.setShiftHz(m_rttyDecoderApplet->rttyShiftHz());
    m_rttyDecoder.setBaudRate(m_rttyDecoderApplet->rttyBaud());
    m_rttyDecoder.setReversePolarity(m_rttyDecoderApplet->rttyReverse());

    if (!m_rttyDecoder.isRunning()) m_rttyDecoder.start();
}


#ifdef HAVE_RADE
void MainWindow::activateRADE(int sliceId)
{
    // Guard against duplicate activation (VfoWidget + RxApplet both selecting RADE)
    if (m_radeSliceId == sliceId && m_radeEngine && m_radeEngine->isActive())
        return;

    // If RADE is already active on a different slice, deactivate it first
    if (m_radeSliceId >= 0 && m_radeSliceId != sliceId)
        deactivateRADE();

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    // Capture TX slice owner before potentially moving the badge, so the
    // failure path can restore it.  -1 means no TX slice existed.
    int prevTxSliceId = sliceId;
    if (!s->isTxSlice()) {
        prevTxSliceId = -1;
        for (auto* sl : m_radioModel.slices()) {
            if (sl && sl->isTxSlice()) { prevTxSliceId = sl->sliceId(); break; }
        }
    }

    // RADE needs to be the TX slice so it can transmit modem audio.
    // Move TX badge to the RADE slice automatically.
    if (!s->isTxSlice())
        s->setTxSlice(true);

    // Set radio mode to DIGU/DIGL (passthrough for OFDM modem).
    // Use band convention from BandDefs to pick sideband — 60m is USB
    // despite being below 10 MHz (#875).
    double freqMhz = s->frequency();
    QString mode = "DIGU";
    for (const auto& band : AetherSDR::kBands) {
        if (freqMhz >= band.lowMhz && freqMhz <= band.highMhz) {
            mode = (QString(band.defaultMode) == "LSB") ? "DIGL" : "DIGU";
            break;
        }
    }
    const QString prevMode       = s->mode();
    const int     prevFilterLow  = s->filterLow();
    const int     prevFilterHigh = s->filterHigh();
    s->setMode(mode);
    if (mode == "DIGL")
        s->setFilterWidth(-2250, -750);
    else
        s->setFilterWidth(750, 2250);

    // Remember which slice and its previous mute state
    m_radeSliceId = sliceId;
    m_radePrevMute = s->audioMute();
    s->setAudioMute(true);

    // Auto-deactivate if the slice mode is changed externally (TCI, profile
    // load, remote SmartSDR client) — those paths bypass the VfoWidget and
    // RxApplet combos and would otherwise leave audio_mute=1 stranded.
    connect(s, &SliceModel::modeChanged,
            this, &MainWindow::onRadeSliceModeChanged,
            Qt::UniqueConnection);

    // Create RADE engine on a worker thread for multi-core utilization
    if (!m_radeEngine) {
        m_radeEngine = new RADEEngine;  // no parent — will be moved to worker thread
        m_radeThread = new QThread(this);
        m_radeThread->setObjectName("RADEEngine");
        m_radeEngine->moveToThread(m_radeThread);
        connect(m_radeThread, &QThread::finished, m_radeEngine, &QObject::deleteLater);
        m_radeThread->start();
    }
    // start() must be invoked on the worker thread
    bool ok = false;
    QMetaObject::invokeMethod(m_radeEngine, [this, &ok]() {
        ok = m_radeEngine->start();
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        qCWarning(lcRade) << "MainWindow: RADE engine failed to start — restoring slice state";
        deactivateRADE();
        if (auto* sl = m_radioModel.slice(sliceId)) {
            sl->setMode(prevMode);
            sl->setFilterWidth(prevFilterLow, prevFilterHigh);
            if (prevTxSliceId != sliceId) {
                if (prevTxSliceId >= 0) {
                    if (auto* prevTx = m_radioModel.slice(prevTxSliceId)) {
                        prevTx->setTxSlice(true);
                    }
                } else {
                    sl->setTxSlice(false);
                }
            }
        }
        return;
    }
    m_radioModel.setDigitalVoiceTxSlice(sliceId);

    // Encode the operator callsign into the EOO frame so the far end can decode it.
    // Resolution order mirrors startFreeDvReporting: radio-stored callsign if
    // "Use radio" is set and non-empty, otherwise the user's saved FreeDV callsign.
    {
        auto& cs = AppSettings::instance();
        QString callsign;
        if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
                && !m_radioModel.callsign().isEmpty()) {
            callsign = m_radioModel.callsign();
        } else {
            callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
        }
        if (!callsign.isEmpty()) {
            QMetaObject::invokeMethod(m_radeEngine, [this, callsign]() {
                m_radeEngine->setTxCallsign(callsign);
            }, Qt::QueuedConnection);
        } else {
            qCWarning(lcRade) << "MainWindow: RADE TX EOO callsign not set — configure callsign in SpotHub FreeDV tab";
        }
    }

    // RADE sends VITA-49 modem audio directly (like TCI), so it needs its own
    // dax_tx stream regardless of platform.  On Windows the ExternalDaxRouteOnly
    // path used by updateDaxTxMode() is intentionally blocked by policy (SmartSDR
    // DAX2 owns the audio-device layer), but that policy doesn't apply here —
    // RADE never touches Windows audio devices.  RadeModemTx is always allowed.
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::RadeModemTx);

    // Only route mic→RADE when the RADE slice IS the TX slice.
    // If another slice is TX (e.g. USB voice), leave its audio path alone.
    m_audio->setRadeMode(s->isTxSlice());

    // TX path: mic -> RADEEngine (worker) -> sendModemTxAudio (main)
    connect(m_audio, &AudioEngine::txRawPcmReady,
            m_radeEngine, &RADEEngine::feedTxAudio, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::txModemReady,
            m_audio, &AudioEngine::sendModemTxAudio, Qt::QueuedConnection);

    // Phase 3: PTT Orchestration — three-layer intercept to hold TX open until
    // the EOO frame has fully played out on the radio before set_mox=0 is sent.
    //
    // Layer 1 — PttOffHook: catches MOX button (TxApplet) and TciServer callers
    // that go through TransmitModel::requestPttOff(). Fires BEFORE setMox(false),
    // so the radio stays in TX while EOO is generated and sent.
    m_radioModel.transmitModel().setPttOffHook([this]() {
        if (m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: PttOffHook — EOO already pending, ignoring duplicate";
            return;
        }
        m_radeEooPending = true;
        syncKiwiSdrTransmitMute();
        qCDebug(lcRade) << "MainWindow: PttOffHook — intercepted requestPttOff, deferring for RADE EOO";
        QMetaObject::invokeMethod(m_radeEngine, [this]() {
            m_radeEngine->setEooRequested(true);
        }, Qt::QueuedConnection);
    });

    // Layer 2 — eooFinished: once EOO audio is queued in AudioEngine, close the
    // audio gate (after EOO packets) then wait for the full EOO playout before
    // dropping the carrier. EOO=144ms + silence=60ms + margin=50ms = 254ms.
    connect(m_radeEngine, &RADEEngine::eooFinished, this, [this]() {
        if (!m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: eooFinished — no pending PTT release (EOO triggered without an intercepted unkey; no carrier to release)";
            return;
        }
        m_radeEooPending = false;
        m_radeTxActive = false;
        syncKiwiSdrTransmitMute();

        // Post setTransmitting(false) AFTER the queued txModemReady(eoo/silence)
        // signals so the audio gate closes only after EOO is in the UDP send buffer.
        if (m_audio)
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setTransmitting(false);
            }, Qt::QueuedConnection);

        constexpr int kEooPlaybackMs = RADEEngine::kEooFrameMs
                                     + RADEEngine::kEooSilenceTailMs
                                     + RADEEngine::kEooTransportMarginMs;
        qCDebug(lcRade) << "MainWindow: RADE eooFinished — deferring xmit 0 by"
                        << kEooPlaybackMs << "ms for EOO playback";
        QTimer::singleShot(kEooPlaybackMs, this, [this]() {
            qCDebug(lcRade) << "MainWindow: RADE EOO playback timer expired — releasing radio PTT";
            m_radioModel.setTransmit(false);
        });
    });

    // Layer 3 — moxChanged fallback: catches interlock and hardware PTT paths
    // (radio-initiated unkey) that bypass both layers above.
    // tx=true: new over starting — clear pending flag and reset engine EOO state.
    // tx=false + !pending + isTransmitting: unintercepted unkey — request EOO as
    //   best-effort (radio may already be in RX, but at least the app won't hang).
    m_radeMoxFallbackConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        if (!m_radeEngine || !m_radeEngine->isActive()) return;
        if (tx) {
            m_radeEooPending = false;
            m_radeTxActive = true;
            syncKiwiSdrTransmitMute();
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->resetTx();
            }, Qt::QueuedConnection);
            qCDebug(lcRade) << "MainWindow: MOX asserted — RADE TX state reset for new over";
        } else if (!m_radeEooPending && m_radeTxActive) {
            qCDebug(lcRade) << "MainWindow: moxChanged(false) fallback — unintercepted PTT release, requesting EOO";
            m_radeEooPending = true;
            syncKiwiSdrTransmitMute();
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->setEooRequested(true);
            }, Qt::QueuedConnection);
        }
    });

    // RX path: DAX RX audio -> RADEEngine (worker) -> decoded speech -> speaker (main)
    // Filter by the RADE slice's DAX channel so other slices' DAX audio is ignored.
    // Look up the channel live so it tracks if the user changes DAX assignment.
    int sid = sliceId;
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_radeEngine, [this, sid](int channel, const QByteArray& pcm) {
        auto* s = m_radioModel.slice(sid);
        if (s && channel == s->daxChannel())
            m_radeEngine->feedRxAudio(channel, pcm);
    }, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::rxSpeechReady,
            m_audio, &AudioEngine::feedDecodedSpeech, Qt::QueuedConnection);

    // Hold the RADE slice's DAX channel via the centralized manager (#3305).
    // It creates the radio-side stream only if no other consumer (TCI, the
    // DAX bridge) already holds the channel; registration comes from
    // RadioModel's central status path — RADE's old private registration hook
    // had no client_handle filter and could adopt a foreign client's stream.
    {
        SliceModel* radeSlice = m_radioModel.slice(sliceId);
        int daxCh = radeSlice ? radeSlice->daxChannel() : 0;
        if (daxCh >= 1 && daxCh <= 4) {
            m_radeDaxChannel = daxCh;
            m_radioModel.panStream()->acquireDaxChannel(
                daxCh, PanadapterStream::DaxConsumer::Rade);
        } else {
            qWarning() << "MainWindow: RADE slice" << sliceId
                       << "has no DAX channel assigned — RX audio will not flow";
        }
        // Follow mid-session dax= changes on the RADE slice: RADE stays active
        // across a channel edit (only a mode change deactivates it), and its
        // RX filter already retargets live via s->daxChannel() — so the Rade
        // hold must move with it or the old channel's stream leaks and the
        // new one is never created. Independent of the DAX bridge's
        // reconciler, which is Bridge-only and absent on Windows.
        // (PR #4017 review item 2)
        if (radeSlice) {
            m_radeDaxReconcileConn = connect(
                radeSlice, &SliceModel::daxChannelChanged,
                this, [this](int newCh) {
                auto* ps = m_radioModel.panStream();
                if (!ps) return;
                const int oldCh = m_radeDaxChannel;
                m_radeDaxChannel = (newCh >= 1 && newCh <= 4) ? newCh : 0;
                if (m_radeDaxChannel)
                    ps->acquireDaxChannel(m_radeDaxChannel,
                                          PanadapterStream::DaxConsumer::Rade);
                if (oldCh >= 1 && oldCh <= 4 && oldCh != m_radeDaxChannel)
                    ps->releaseDaxChannel(oldCh,
                                          PanadapterStream::DaxConsumer::Rade);
                qCInfo(lcRade) << "MainWindow: RADE dax hold moved"
                               << oldCh << "->" << m_radeDaxChannel;
            });
        }
    }

    // Restore client-side mic gain for RADE. The radio's mic input is unused in
    // RADE mode so PcMicGain applies regardless of the current mic_selection.
    {
        int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
        m_audio->setPcMicGain(gain);
    }
    m_appletPanel->phoneCwApplet()->setRadeActive(true);

    // Start mic capture if not already running
    if (!m_audio->isTxStreaming()) {
        audioStartTx(m_radioModel.radioAddress(), 4991);
    }

    // RADE status indicator in VFO widget.
    // Use spectrumForSlice() to find the correct pan — in multi-pan layouts
    // spectrum() returns the *active* pan, which may not be the RADE slice's pan.
    if (auto* sw = spectrumForSlice(m_radioModel.slice(sliceId))) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true);
            // Show initial unsynchronised state immediately — syncChanged only fires
            // from feedRxAudio() which requires DAX audio to be flowing first.
            vfo->setRadeSynced(false);
            connect(m_radeEngine, &RADEEngine::syncChanged,
                    vfo, &VfoWidget::setRadeSynced);
            connect(m_radeEngine, &RADEEngine::snrChanged,
                    vfo, &VfoWidget::setRadeSnr);
            connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                    vfo, &VfoWidget::setRadeFreqOffset);
            connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                    vfo, &VfoWidget::setRadeCallsign, Qt::QueuedConnection);
        }
    }

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true);
        applet->setRadeSynced(false);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                applet, &RadeApplet::setRadeSynced);
        connect(m_radeEngine, &RADEEngine::snrChanged,
                applet, &RadeApplet::setRadeSnr);
        connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                applet, &RadeApplet::setRadeFreqOffset);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                applet, &RadeApplet::setRadeCallsign, Qt::QueuedConnection);
    }

    // Store far-end callsign received in EOO frame for display / future use.
    connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
            this, [this](const QString& callsign) {
                m_lastRadeRxCallsign = callsign;
                qCDebug(lcRade) << "RADE EOO callsign received:" << callsign;
            });

    // FreeDV Reporter: station reporting when the user has opted in.
    if (AppSettings::instance().value("FreeDvAutoReport", "False").toString() == "True")
        startFreeDvReporting(sliceId);

    qInfo() << "MainWindow: RADE mode activated on slice" << sliceId;
}

void MainWindow::deactivateRADE()
{
    // Capture slice ID before any field mutations below clear it.
    const int radeSliceId = m_radeSliceId;

    // Restore audio mute state on the RADE slice
    if (m_radeSliceId >= 0) {
        if (auto* s = m_radioModel.slice(m_radeSliceId)) {
            disconnect(s, &SliceModel::modeChanged,
                       this, &MainWindow::onRadeSliceModeChanged);
            s->setAudioMute(m_radePrevMute);
        }
        // Clear RADE status label and disconnect VFO signal connections before resetting sliceId.
        // Do this here (with m_radeSliceId still valid) rather than in the m_radeEngine block
        // below where the slice ID has already been cleared.
        if (auto* sw = spectrumForSlice(m_radioModel.slice(m_radeSliceId))) {
            if (auto* vfo = sw->vfoWidget(m_radeSliceId)) {
                vfo->setRadeActive(false);
                if (m_radeEngine) {
                    disconnect(m_radeEngine, &RADEEngine::syncChanged,         vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::snrChanged,           vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,    vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  vfo, nullptr);
                }
            }
        }
        m_radeSliceId = -1;
    }

    m_radioModel.transmitModel().clearPttOffHook();
    disconnect(m_radeMoxFallbackConn);
    m_radeEooPending = false;
    m_radeTxActive = false;
    syncKiwiSdrTransmitMute();

    m_audio->setRadeMode(false);
    m_radioModel.setDigitalVoiceTxSlice(-1);
    m_audio->clearTxAccumulators();  // flush stale RADE modem data
    m_appletPanel->phoneCwApplet()->setRadeActive(false);

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(false);
        if (m_radeEngine) {
            disconnect(m_radeEngine, &RADEEngine::syncChanged,        applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::snrChanged,         applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,  applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived, applet, nullptr);
        }
    }

    // For hardware mics, reset to full gain — the radio controls hardware levels.
    // PC mic keeps its PcMicGain so SSB sessions are unaffected.
    if (m_radioModel.transmitModel().micSelection() != "PC") {
        m_audio->setPcMicGain(100);
    }
    m_lastRadeRxCallsign.clear();

    if (m_radeEngine) {
        disconnect(m_audio, &AudioEngine::txRawPcmReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::txModemReady,
                   m_audio, nullptr);
        disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::rxSpeechReady,
                   m_audio, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooFinished,
                   this, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                   this, nullptr);
        disconnect(m_radeDaxReconcileConn);
        if (m_radeDaxChannel >= 1 && m_radeDaxChannel <= 4) {
            // Release RADE's hold; the centralized manager removes the
            // radio-side stream only when the LAST holder (TCI / DAX bridge /
            // RADE) releases — the ref-counting the old TODO here asked for
            // (#3305).
            m_radioModel.panStream()->releaseDaxChannel(
                m_radeDaxChannel, PanadapterStream::DaxConsumer::Rade);
            m_radeDaxChannel = 0;
        }
        // Stop on the worker thread, then shut down the thread
        QMetaObject::invokeMethod(m_radeEngine, &RADEEngine::stop,
                                  Qt::BlockingQueuedConnection);
        if (m_radeThread) {
            m_radeThread->quit();
            m_radeThread->wait(2000);
            m_radeThread->deleteLater();
            m_radeThread = nullptr;
        }
        m_radeEngine = nullptr;  // deleteLater handles actual deletion
    }

    stopFreeDvReporting(radeSliceId);

    qInfo() << "MainWindow: RADE mode deactivated";
}

void MainWindow::onRadeSliceModeChanged(const QString& mode)
{
    // Fired when the RADE slice's mode changes via any path — TCI, profile
    // load, remote SmartSDR client. RADE requires DIGU or DIGL; deactivate
    // if the mode leaves that family so audio_mute is always restored.
    if (mode != "DIGU" && mode != "DIGL")
        deactivateRADE();
}

void MainWindow::activateFdvDisplay(int sliceId)
{
    if (m_fdvDisplaySliceId == sliceId)
        return;

    if (m_fdvDisplaySliceId >= 0)
        deactivateFdvDisplay();

    m_fdvDisplaySliceId = sliceId;

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    if (auto* sw = spectrumForSlice(s)) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true, QStringLiteral("FreeDV"));
            vfo->setRadeSynced(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true, QStringLiteral("FreeDV"));
        applet->setRadeSynced(false);
    }
#endif

    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");

    connect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
            this, &MainWindow::onFdvMeterUpdated,
            Qt::UniqueConnection);
    connect(&m_radioModel, &RadioModel::metersChanged,
            this, &MainWindow::onFdvMetersChanged,
            Qt::UniqueConnection);

    m_fdvSynced = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display activated on slice" << sliceId;
}

void MainWindow::deactivateFdvDisplay()
{
    if (m_fdvDisplaySliceId < 0)
        return;

    if (auto* s = m_radioModel.slice(m_fdvDisplaySliceId)) {
        if (auto* sw = spectrumForSlice(s)) {
            if (auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId))
                vfo->setRadeActive(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet())
        applet->setRadeActive(false);
#endif

    disconnect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
               this, &MainWindow::onFdvMeterUpdated);
    disconnect(&m_radioModel, &RadioModel::metersChanged,
               this, &MainWindow::onFdvMetersChanged);

    m_fdvDisplaySliceId = -1;
    m_fdvSnrMeterIndex  = -1;
    m_fdvSynced         = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display deactivated";
}

void MainWindow::onFdvMeterUpdated(int index, float value)
{
    if (index != m_fdvSnrMeterIndex || m_fdvDisplaySliceId < 0)
        return;

    auto* s = m_radioModel.slice(m_fdvDisplaySliceId);
    if (!s) return;
    auto* sw = spectrumForSlice(s);
    if (!sw) return;
    auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId);
    if (!vfo) return;

    const bool synced = (value > -98.9f);
    if (synced != m_fdvSynced) {
        m_fdvSynced = synced;
        vfo->setRadeSynced(synced);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSynced(synced);
#endif
    }
    if (synced) {
        vfo->setRadeSnr(value);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSnr(value);
#endif
    }
}

void MainWindow::onFdvMetersChanged()
{
    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");
}

void MainWindow::startFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    // RADE without WebSockets: reporter client doesn't exist, no-op. (#2204)
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    auto& cs = AppSettings::instance();

    // Callsign: prefer radio-stored value if "Use radio" is set and it's populated.
    QString callsign;
    if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
            && !m_radioModel.callsign().isEmpty()) {
        callsign = m_radioModel.callsign();
    } else {
        callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
    }

    // Grid: GPS (if hardware present, locked, and user prefers it), else user-saved.
    QString grid;
    if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
            && m_radioModel.hasGpsHardware()
            && !m_radioModel.gpsGrid().isEmpty()) {
        grid = m_radioModel.gpsGrid();
    } else {
        grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
    }

    // Refuse to broadcast placeholder data to the public FreeDV Reporter
    // map — the dialog already validates this when the user toggles the
    // checkbox, but RADE auto-activation can hit this path without going
    // through the toggle, so guard here too.
    if (callsign.isEmpty() || grid.isEmpty()) {
        qCWarning(lcDxCluster)
            << "FreeDvReporting: refusing to enable — callsign or grid empty"
            << "(callsign='" << callsign << "', grid='" << grid << "')";
        return;
    }

    const QString message = cs.value("FreeDvMyMessage", "").toString();
    const QString swVer   = QString("AetherSDR %1").arg(QCoreApplication::applicationVersion());
    const double  freqMhz = m_radioModel.slice(sliceId)
                            ? m_radioModel.slice(sliceId)->frequency() : 0.0;

    // Auto-start the WebSocket connection if not already running — reporting is
    // independent of the user's FreeDV spot subscription.
    if (!m_freedvClient->isConnected())
        QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->startConnection(); });

    QMetaObject::invokeMethod(m_freedvClient,
        [this, callsign, grid, message, swVer, freqMhz] {
            m_freedvClient->enableReporting(callsign, grid, message, swVer, freqMhz);
        });

    // TX report: evaluate tune/ATU guard on the main thread so we never report
    // a tune or ATU cycle as a voice transmission.
    m_freedvMoxConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
                const TransmitModel& txm = m_radioModel.transmitModel();
                if (txm.isTuning() || txm.atuStatus() == ATUStatus::InProgress)
                    return;
                QMetaObject::invokeMethod(m_freedvClient, [this, tx] {
                    m_freedvClient->reportTxState(tx);
                });
            });

    if (m_radeEngine) {
        connect(m_radeEngine, &RADEEngine::snrChanged,
                m_freedvClient, &FreeDvClient::updateRxSnr, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                m_freedvClient, &FreeDvClient::updateRxSynced, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                m_freedvClient, &FreeDvClient::updateRxCallsign, Qt::QueuedConnection);
    }
    if (SliceModel* radeSlice = m_radioModel.slice(sliceId)) {
        connect(radeSlice, &SliceModel::frequencyChanged,
                m_freedvClient, &FreeDvClient::reportFreqChange, Qt::QueuedConnection);
    }
#endif  // HAVE_WEBSOCKETS
}

void MainWindow::stopFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    disconnect(m_freedvMoxConn);
    if (m_radeEngine) {
        disconnect(m_radeEngine, &RADEEngine::snrChanged,           m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::syncChanged,          m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  m_freedvClient, nullptr);
    }
    if (auto* radeSlice = m_radioModel.slice(sliceId))
        disconnect(radeSlice, &SliceModel::frequencyChanged, m_freedvClient, nullptr);

    QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->disableReporting(); });
#endif
}

#endif  // HAVE_RADE

#ifdef HAVE_WEBSOCKETS
void MainWindow::showFreeDvReporter()
{
    if (!m_freedvReporterDialog) {
        m_freedvReporterDialog = new FreeDvReporterDialog(this);
        connect(m_freedvClient, &FreeDvClient::stationsCleared,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationsCleared,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationUpdated,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationUpdated,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationRemoved,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationRemoved,
                Qt::QueuedConnection);
        if (auto* s = activeSlice())
            m_freedvReporterDialog->setActiveSlice(s);
        // Seed with current state — bulk_update fires at connect time, before the
        // dialog exists. Without this, the table fills slowly from live events only.
        for (const auto& [sid, info] : m_freedvClient->stations().asKeyValueRange())
            m_freedvReporterDialog->onStationUpdated(sid, info);
    }
    // Resolve GPS-aware grid every open — same logic as startFreeDvReporting()
    // so km/Hdg columns work when GPS grid is active and never written to AppSettings.
    {
        auto& cs = AppSettings::instance();
        QString grid;
        if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
                && m_radioModel.hasGpsHardware()
                && !m_radioModel.gpsGrid().isEmpty()) {
            grid = m_radioModel.gpsGrid();
        } else {
            grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
        }
        if (grid.isEmpty())
            grid = m_freedvClient->myGrid();
        m_freedvReporterDialog->setMyGrid(grid);
    }
    m_freedvReporterDialog->show();
    m_freedvReporterDialog->raise();
    m_freedvReporterDialog->activateWindow();
}
#endif

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
bool MainWindow::startDax()
{
    if (m_daxBridge) return true;

#ifdef Q_OS_MAC
    // Only start if the macOS HAL driver bundle is installed.
    if (!macDaxDriverInstalled()) {
        qWarning() << "MainWindow: DAX HAL plugin not installed";
        QMessageBox::warning(this, "DAX Audio Driver Missing",
            "The AetherSDR DAX audio driver is not installed on this Mac.\n\n"
            "Install the DAX Virtual Audio Driver from the AetherSDR DMG package, "
            "then enable DAX again.");
        return false;
    }
#endif

    m_daxBridge = new DaxBridge(this);
    if (!m_daxBridge->open()) {
        qWarning() << "MainWindow: failed to open DAX audio bridge";
        QMessageBox::warning(this, "DAX Audio Bridge Error",
            "AetherSDR could not open the DAX audio bridge.\n\n"
            "If the DAX driver was just installed, quit and relaunch AetherSDR and try again.");
        delete m_daxBridge;
        m_daxBridge = nullptr;
        return false;
    }

    // Stream status registration now lives in ONE place —
    // RadioModel::handleDaxRxStreamRegistry (#3305) — instead of per-consumer
    // statusReceived hooks with divergent filtering.

    // Acquire DAX channels only for slices with a channel assigned.
    // FlexLib creates streams on demand, not all 4 unconditionally.
    // Creating unused streams causes the radio to round-robin audio
    // across all of them, starving the active channels.
    m_daxSliceLastCh.clear();
    for (auto* s : m_radioModel.slices()) {
        int ch = s->daxChannel();
        m_daxSliceLastCh[s->sliceId()] = ch;
        if (ch >= 1 && ch <= 4) {
            m_radioModel.panStream()->acquireDaxChannel(
                ch, PanadapterStream::DaxConsumer::Bridge);
        }
    }

    // #2895: the one-shot loop above only covers slices that ALREADY have a
    // DAX channel at bridge startup (typically just slice 0 / DAX 1). When the
    // user later assigns DAX 2-4 to another slice via the UI, SliceModel only
    // sends `slice set <id> dax=<ch>` — it never sends `stream create
    // type=dax_rx`, so the radio never registers a DAX client (dax_clients
    // stays 0) and sends silence. React to per-slice channel changes here and
    // create/remove the DAX RX stream on demand, mirroring the TCI path
    // (TciServer::ensureDaxForTci, #1331/#1439) and FlexLib
    // RequestDAXRXAudioStream(channel).
    for (auto* s : m_radioModel.slices()) {
        wireDaxSlice(s);
    }
    m_daxSliceConns.append(connect(&m_radioModel, &RadioModel::sliceAdded,
                                   this, [this](SliceModel* s) {
        if (!m_daxBridge || !s) return;
        // Let onDaxChannelChanged() see a 0 -> channel transition for slices
        // restored with DAX already assigned.
        m_daxSliceLastCh[s->sliceId()] = 0;
        wireDaxSlice(s);
        // A slice can arrive already carrying a DAX channel (radio profile
        // restore); make sure its stream exists too.
        if (s->daxChannel() >= 1 && s->daxChannel() <= 4) {
            onDaxChannelChanged(s, s->daxChannel());
        }
    }));
    // A slice removed out from under us (pan close, foreign client, profile
    // load) never fires daxChannelChanged — release its channel here or the
    // bridge holds it until stopDax (a silent radio-side orphan, #3305).
    m_daxSliceConns.append(connect(&m_radioModel, &RadioModel::sliceRemoved,
                                   this, [this](int sliceId) {
        if (!m_daxBridge) return;
        const int ch = m_daxSliceLastCh.take(sliceId);
        if (ch < 1 || ch > 4) return;
        for (auto* s : m_radioModel.slices()) {
            if (s && s->daxChannel() == ch) return;  // channel hopped slices
        }
        m_radioModel.panStream()->releaseDaxChannel(
            ch, PanadapterStream::DaxConsumer::Bridge);
    }));

    // Wire DAX RX: PanadapterStream routes registered DAX streams here
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_daxBridge, &DaxBridge::feedDaxAudio);

    // DAX-IQ stream-status routing, the VITA-49 IQ feed, level meter, and the
    // enable/disable/rate connections are wired ONCE at construction (see the
    // "DAX IQ wiring (all platforms)" block in the constructor) — they are
    // independent of the audio bridge and must outlive a DAX-audio stop, so
    // they are deliberately NOT wired here. Persisted-enabled IQ channels are
    // restored by the applet itself on connect (restoreEnabledChannels), which
    // now runs after the construction-time wiring on every platform.

    // Wire DAX level meters
    connect(m_daxBridge, &DaxBridge::daxRxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxRxLevel);
    connect(m_daxBridge, &DaxBridge::daxTxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxTxLevel);

    // Wire DAX gain sliders
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxRxGainChanged,
            m_daxBridge, &DaxBridge::setChannelGain);
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxTxGainChanged,
            m_daxBridge, &DaxBridge::setTxGain);

    // Apply saved gains to the bridge
    auto& ss = AppSettings::instance();
    for (int i = 1; i <= 4; ++i)
        m_daxBridge->setChannelGain(i, ss.value(QStringLiteral("DaxRxGain%1").arg(i), "0.5").toString().toFloat());
    m_daxBridge->setTxGain(ss.value("DaxTxGain", "0.5").toString().toFloat());

    // Wire DAX TX: apps → bridge → AudioEngine → VITA-49.
    // AudioEngine chooses packet format/routing based on DaxTxLowLatency.
    connect(m_daxBridge, &DaxBridge::txAudioReady,
            this, [this](const QByteArray& pcm) {
        if (m_audio->isRadeMode()) return;
        if (!m_audio->isDaxTxMode()) return;
        QMetaObject::invokeMethod(m_audio, [this, pcm]() { m_audio->feedDaxTxAudio(pcm); });
    });

    // Save current mic selection before forcing PC audio source.
    m_savedMicSelection = m_radioModel.transmitModel().micSelection();

    // Default to the radio-native DAX route (dax=1, int16 mono).  RADE
    // mode overrides this to the low-latency route via setRadeMode()
    // when the user enters RADE — see AudioEngine::setRadeMode().
    m_audio->setDaxTxUseRadioRoute(true);
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
    m_radioModel.sendCommand("transmit set mic_selection=PC");
    // Don't force dax=1 here — radio-side DAX flag follows mode changes
    // via updateDaxTxMode(). Bridge up ≠ DAX TX active. (#534)

    qInfo() << "MainWindow: starting DAX audio bridge";
    return true;
}

void MainWindow::stopDax()
{
    if (!m_daxBridge) return;

    m_audio->setDaxTxMode(false);
    m_audio->clearTxAccumulators();

    // #2895: drop the per-slice daxChannelChanged / sliceAdded reactions wired
    // in startDax() so they don't fire against a torn-down bridge.
    for (const auto& c : m_daxSliceConns) {
        disconnect(c);
    }
    m_daxSliceConns.clear();
    m_daxSliceLastCh.clear();

    disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
               m_daxBridge, nullptr);
    disconnect(m_daxBridge, &DaxBridge::txAudioReady,
               this, nullptr);

    // Release every channel the bridge holds. PanadapterStream removes a
    // radio-side stream only when the LAST holder releases, so a channel TCI
    // or RADE still uses survives a bridge teardown — the #3363/#2886 failure
    // class, now enforced structurally instead of by cross-consumer peeking
    // (#3305).
    m_radioModel.panStream()->releaseAllDaxChannels(
        PanadapterStream::DaxConsumer::Bridge);

    // Restore original mic selection
    if (!m_savedMicSelection.isEmpty() && m_savedMicSelection != "PC")
        m_radioModel.sendCommand(QString("transmit set mic_selection=%1").arg(m_savedMicSelection));

    m_daxBridge->close();
    delete m_daxBridge;
    m_daxBridge = nullptr;
    qInfo() << "MainWindow: stopping DAX audio bridge";
}

// #2895: connect one slice's daxChannelChanged so a DAX channel (re)assigned
// after the bridge is already up still gets a radio-side DAX RX stream.
void MainWindow::wireDaxSlice(SliceModel* slice)
{
    if (!slice) return;
    m_daxSliceConns.append(connect(slice, &SliceModel::daxChannelChanged,
                                   this, [this, slice](int newCh) {
        if (!m_daxBridge) return;  // bridge torn down — ignore late signals
        onDaxChannelChanged(slice, newCh);
    }));
}

// Handle a slice's DAX channel transitioning. daxChannelChanged only fires on
// an actual value change (SliceModel guards equality), and arrives from both
// the local UI setter and the radio status echo.
//
// #3305/#4009: this reconciler translates slice→channel transitions into
// refcounted acquire/release on PanadapterStream and NOTHING ELSE. It must
// never send commands itself: the radio answers a re-assert of a live binding
// with a transient unbind/rebind dax=0/dax=<ch> status pair
// (state-machines.md §7.4), so any command emitted from this echo path feeds
// a self-sustaining storm — the ~12-15 Hz `slice set dax` loop of #4009 and
// the create/remove churn of #3626. Acquire/release are idempotent and the
// manager's grace window absorbs the transient pair, so this path is
// loop-free by construction. The #1439 dax_clients re-assert still exists,
// but as a one-shot tied to `stream create` in RadioModel — command-initiated,
// never echo-initiated.
void MainWindow::onDaxChannelChanged(SliceModel* slice, int newCh)
{
    if (!slice || !m_daxBridge) return;
    auto* ps = m_radioModel.panStream();
    if (!ps) return;

    const int sliceId = slice->sliceId();
    const int oldCh = m_daxSliceLastCh.value(sliceId, 0);
    if (oldCh == newCh) return;
    // Record every transition, including 0: the tracker mirrors the slice's
    // channel so a genuine off→on retoggle is seen as 0→N and re-acquires.
    m_daxSliceLastCh[sliceId] = newCh;

    if (newCh >= 1 && newCh <= 4)
        ps->acquireDaxChannel(newCh, PanadapterStream::DaxConsumer::Bridge);
    if (oldCh >= 1 && oldCh <= 4) {
        // The Bridge's hold is per-channel, not per-slice: only release when
        // no slice references the old channel anymore (a channel can hop
        // between slices, and the moves can arrive in either order).
        bool stillWanted = false;
        for (auto* s : m_radioModel.slices()) {
            if (s && s->daxChannel() == oldCh) { stillWanted = true; break; }
        }
        if (!stillWanted)
            ps->releaseDaxChannel(oldCh, PanadapterStream::DaxConsumer::Bridge);
    }
}

// (#3626's deferred-removal + revalidation and #2895's cross-consumer
// ownership guards were absorbed into PanadapterStream's refcounted DAX
// channel manager — grace-window removal at last-holder release, per-consumer
// holds instead of tciUsing/radeUsing peeking. #3305)
#endif

// registerMidiParams() lives in MainWindow_Controllers.cpp (#3351 Phase 1a).

// ─── WFM software demodulator ────────────────────────────────────────────────
// Relocated here from MainWindow.cpp (#3407 follow-up). Per-slice software FM
// demod activated by the RxApplet/VfoWidget WFM toggles (those connects stay in
// MainWindow.cpp beside the RADE ones). Same activate/deactivate-overlay shape
// as activateRADE/deactivateRADE above.

void MainWindow::activateWFM(int sliceId)
{
    if (m_wfmSliceId == sliceId) return;
    deactivateWFM();

    m_wfmCooldown = true;
    QTimer::singleShot(1000, this, [this]{ m_wfmCooldown = false; });

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    auto resolveAudioDevice = [this]() -> QString {
        QString deviceId = WfmSettings::audioDeviceId().trimmed();
        if (!deviceId.isEmpty())
            return deviceId;
        WfmDeviceDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted || dlg.selectedDeviceId().isEmpty())
            return {};   // user cancelled
        deviceId = dlg.selectedDeviceId();
        if (dlg.rememberChoice())
            WfmSettings::setAudioDeviceId(deviceId);
        return deviceId;
    };

    const QString audioDeviceId = resolveAudioDevice();
    if (audioDeviceId.isEmpty()) {
        m_wfmSliceId = -1;
        reflectWfmButtons(false, sliceId);   // un-stick the button that triggered us
        return;
    }

    m_wfmPrevFilterLo = s->filterLow();
    m_wfmPrevFilterHi = s->filterHigh();
    s->setFilterWidth(-WfmDemodulator::FILTER_HZ, WfmDemodulator::FILTER_HZ);
    m_wfmSliceId = sliceId;

    // Centre the pan (and with it the DAX IQ stream) on the slice — once.
    // applyPanStatus updates the local model immediately so offsets computed
    // before the radio echoes the new centre are already correct.
    auto centerPanAtSlice = [this, s]() {
        const QString panId = s->panId();
        if (panId.isEmpty()) return;
        const double freq = s->frequency();
        auto* pan = m_radioModel.panadapter(panId);
        if (pan && qFuzzyCompare(pan->centerMhz(), freq)) return;
        const QString freqStr = QString::number(freq, 'f', 6);
        if (pan) pan->setCenterBandwidth(freq, -1.0);  // aetherd RFC 2.3
        m_radioModel.sendCommand(
            QString("display pan set %1 center=%2").arg(panId, freqStr));
    };
    centerPanAtSlice();

    m_wfmDemod = new WfmDemodulator(this);
    connect(m_wfmDemod, &WfmDemodulator::commandReady,
            &m_radioModel, &RadioModel::sendCommand);
    m_wfmDemod->setVolume(static_cast<int>(s->audioGain()));
    connect(s, &SliceModel::audioGainChanged,
            m_wfmDemod, [demod = m_wfmDemod](float g) { demod->setVolume(static_cast<int>(g)); });
    m_wfmDemod->start(&m_radioModel.daxIqModel(), audioDeviceId, s->panId());
    if (!m_wfmDemod->isActive()) {
        WfmSettings::clearAudioDeviceId();
        delete m_wfmDemod;
        m_wfmDemod = nullptr;
        m_wfmSliceId = -1;
        reflectWfmButtons(false, sliceId);   // un-stick the button that triggered us
        return;
    }

    // Demod is live — reflect the real state onto both UI surfaces so the
    // surface that did NOT initiate (and any mode combo) tracks it too.
    reflectWfmButtons(true, sliceId);

    // SkyRoof policy: Doppler rides the demodulator's NCO while the pan (and
    // the DAX IQ centre) stays put — each retune is phase-continuous, so the
    // modem never unlocks and the pan is never yanked. Recentre only when the
    // slice would leave the usable IQ window (rare: at 70 cm the Doppler
    // swing is ±10 kHz vs a ±14.8 kHz window at 48 k).
    m_wfmFreqConn = connect(s, &SliceModel::frequencyChanged,
                            this, [this, s, centerPanAtSlice](double sliceFreqMhz) {
        if (!m_wfmDemod) return;
        auto* pan = m_radioModel.panadapter(s->panId());
        if (!pan) return;
        const float offsetHz = static_cast<float>(
            (sliceFreqMhz - pan->centerMhz()) * 1e6);
        if (qAbs(offsetHz) <= m_wfmDemod->maxFreqOffsetHz()) {
            m_wfmDemod->setFreqOffsetHz(offsetHz);
        } else {
            centerPanAtSlice();
            m_wfmDemod->setFreqOffsetHz(0.0f);
        }
    });
}

// Always cleans up — the cooldown debounce lives in the wfmActivated
// handlers, NOT here, so activateWFM's switch-slice path can never leak a
// running demodulator (two demods writing to the same VAC device).
void MainWindow::deactivateWFM()
{
    if (m_wfmSliceId < 0) return;

    const int deactivatedSliceId = m_wfmSliceId;

    disconnect(m_wfmFreqConn);

    if (m_wfmDemod) {
        delete m_wfmDemod;
        m_wfmDemod = nullptr;
    }

    if (auto* s = m_radioModel.slice(m_wfmSliceId)) {
        if (m_wfmPrevFilterLo != 0 || m_wfmPrevFilterHi != 0)
            s->setFilterWidth(m_wfmPrevFilterLo, m_wfmPrevFilterHi);
    }

    m_wfmSliceId = -1;
    m_wfmPrevFilterLo = 0;
    m_wfmPrevFilterHi = 0;

    reflectWfmButtons(false, deactivatedSliceId);
}

// Mirror the real demod state onto the WFM toggle in the spectrum overlay DAX
// menu (the single WFM surface). It self-gates on its own slice, so passing the
// affected sliceId is enough; setWfmActive() blocks the button's signal, so this
// never loops back through the wfm handler.
void MainWindow::reflectWfmButtons(bool on, int sliceId)
{
    if (auto* sw = spectrumForSlice(m_radioModel.slice(sliceId))) {
        if (auto* menu = sw->overlayMenu())
            menu->setWfmActive(on, sliceId);
    }
}

void MainWindow::showPskReporterMapDialog()
{
    if (!m_pskReporterMapDialog) {
        auto* dlg = new PskReporterMapDialog(&m_radioModel, m_propForecast, this);
        dlg->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        m_pskReporterMapDialog = dlg;
        m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
    }
    m_pskReporterMapDialog->show();
    m_pskReporterMapDialog->raise();
    m_pskReporterMapDialog->activateWindow();
}

} // namespace AetherSDR
